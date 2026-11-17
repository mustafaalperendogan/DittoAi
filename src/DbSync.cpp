#include "stdafx.h"
#include "DbSync.h"
#include "AiJson.h"
#include "AiConfig.h"
#include "CP_Main.h"
#include "Clip.h"
#include "Misc.h"
#include "Options.h"

#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <memory>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace
{
	const UINT WM_DB_SYNC_DONE = WM_APP + 162;		// worker -> controller window, lParam = DbSyncResult*

	const int PACKAGE_VERSION = 1;
	const ULONG KDF_ITERATIONS = 210000;
	const __int64 TOMBSTONE_KEEP_SECONDS = 180LL * 24 * 3600;
	const int MAX_ATTEMPTS = 3;						// the upload is retried when another computer uploaded first
	const LPCTSTR UNICODE_TEXT = _T("CF_UNICODETEXT");

	typedef std::vector<BYTE> Bytes;

	__int64 Now()
	{
		return (__int64)_time64(NULL);
	}

	CString FormatTime(__int64 t)
	{
		return t > 0 ? CTime((time_t)t).Format(_T("%d.%m.%Y %H:%M")) : CString(_T("-"));
	}

	CString ComputerName()
	{
		wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
		DWORD size = _countof(name);
		::GetComputerNameW(name, &size);
		return name;
	}

	// ---------------------------------------------------------------- encoding, hashing, encryption

	std::string Base64Encode(const BYTE *data, size_t size)
	{
		if (size == 0)
		{
			return std::string();
		}
		DWORD chars = 0;
		::CryptBinaryToStringA(data, (DWORD)size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &chars);
		std::string out(chars, '\0');
		if (!::CryptBinaryToStringA(data, (DWORD)size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &out[0], &chars))
		{
			return std::string();
		}
		out.resize(chars);
		return out;
	}

	bool Base64Decode(const std::string &text, Bytes &out)
	{
		out.clear();
		if (text.empty())
		{
			return true;
		}
		DWORD size = 0;
		if (!::CryptStringToBinaryA(text.c_str(), (DWORD)text.size(), CRYPT_STRING_BASE64, NULL, &size, NULL, NULL))
		{
			return false;
		}
		out.resize(size);
		if (!::CryptStringToBinaryA(text.c_str(), (DWORD)text.size(), CRYPT_STRING_BASE64, out.data(), &size, NULL, NULL))
		{
			return false;
		}
		out.resize(size);
		return true;
	}

	std::string Hex(const BYTE *data, size_t size)
	{
		static const char digits[] = "0123456789abcdef";
		std::string out;
		out.reserve(size * 2);
		for (size_t i = 0; i < size; i++)
		{
			out += digits[data[i] >> 4];
			out += digits[data[i] & 15];
		}
		return out;
	}

	class Sha256
	{
	public:
		Sha256()
		{
			if (BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
			{
				::BCryptCreateHash(m_alg, &m_hash, NULL, 0, NULL, 0, 0);
			}
		}
		~Sha256()
		{
			if (m_hash) ::BCryptDestroyHash(m_hash);
			if (m_alg) ::BCryptCloseAlgorithmProvider(m_alg, 0);
		}

		void Add(const void *data, size_t size)
		{
			if (m_hash != NULL && size > 0)
			{
				::BCryptHashData(m_hash, (PUCHAR)data, (ULONG)size, 0);
			}
		}

		// length prefixed, so ("ab", "c") and ("a", "bc") differ
		void AddField(const void *data, size_t size)
		{
			UINT64 length = size;
			Add(&length, sizeof(length));
			Add(data, size);
		}

		void AddField(const CString &text)
		{
			std::string utf8 = AiToUtf8(text);
			AddField(utf8.data(), utf8.size());
		}

		std::string HexDigest()
		{
			BYTE digest[32] = { 0 };
			if (m_hash != NULL)
			{
				::BCryptFinishHash(m_hash, digest, sizeof(digest), 0);
			}
			return Hex(digest, sizeof(digest));
		}

	private:
		BCRYPT_ALG_HANDLE m_alg = NULL;
		BCRYPT_HASH_HANDLE m_hash = NULL;
	};

	Bytes RandomBytes(size_t size)
	{
		Bytes out(size);
		::BCryptGenRandom(NULL, out.data(), (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
		return out;
	}

	CString NewId()
	{
		Bytes bytes = RandomBytes(16);
		return CString(Hex(bytes.data(), bytes.size()).c_str());
	}

	struct CryptoKey
	{
		Bytes salt;
		ULONG iterations = 0;
		Bytes key;
	};

	bool DeriveKey(const CString &password, CryptoKey &key)
	{
		std::string utf8 = AiToUtf8(password);
		BCRYPT_ALG_HANDLE prf = NULL;
		if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&prf, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
		{
			return false;
		}
		key.key.assign(32, 0);
		NTSTATUS status = ::BCryptDeriveKeyPBKDF2(prf, (PUCHAR)utf8.data(), (ULONG)utf8.size(), key.salt.data(), (ULONG)key.salt.size(),
			key.iterations, key.key.data(), (ULONG)key.key.size(), 0);
		::BCryptCloseAlgorithmProvider(prf, 0);
		::SecureZeroMemory(&utf8[0], utf8.size());
		if (!BCRYPT_SUCCESS(status))
		{
			key.key.clear();
			return false;
		}
		return true;
	}

	// AES-256-GCM. Encrypting fills 'tag'; decrypting checks it (false = wrong password or damaged data).
	bool AesGcm(bool encrypt, const Bytes &key, const Bytes &nonce, const Bytes &input, Bytes &output, Bytes &tag)
	{
		BCRYPT_ALG_HANDLE alg = NULL;
		BCRYPT_KEY_HANDLE hKey = NULL;
		bool ok = false;

		if (BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0)) &&
			BCRYPT_SUCCESS(::BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) &&
			BCRYPT_SUCCESS(::BCryptGenerateSymmetricKey(alg, &hKey, NULL, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0)))
		{
			if (encrypt)
			{
				tag.assign(16, 0);
			}
			BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
			BCRYPT_INIT_AUTH_MODE_INFO(info);
			info.pbNonce = (PUCHAR)nonce.data();
			info.cbNonce = (ULONG)nonce.size();
			info.pbTag = tag.data();
			info.cbTag = (ULONG)tag.size();

			output.assign(input.size(), 0);
			ULONG written = 0;
			NTSTATUS status = encrypt
				? ::BCryptEncrypt(hKey, (PUCHAR)input.data(), (ULONG)input.size(), &info, NULL, 0, output.data(), (ULONG)output.size(), &written, 0)
				: ::BCryptDecrypt(hKey, (PUCHAR)input.data(), (ULONG)input.size(), &info, NULL, 0, output.data(), (ULONG)output.size(), &written, 0);
			ok = BCRYPT_SUCCESS(status);
			output.resize(ok ? written : 0);
		}

		if (hKey) ::BCryptDestroyKey(hKey);
		if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
		return ok;
	}

	// Envelope stored in the cloud: {"format":"ditto-sync", ..., "data": base64(AES-GCM(package JSON))}
	std::string EncryptPackage(const std::string &json, const CString &password, CryptoKey &key)
	{
		if (key.key.empty())
		{
			key.salt = RandomBytes(16);
			key.iterations = KDF_ITERATIONS;
			if (!DeriveKey(password, key))
			{
				return std::string();
			}
		}

		Bytes nonce = RandomBytes(12);
		Bytes plain(json.begin(), json.end());
		Bytes cipher, tag;
		bool ok = AesGcm(true, key.key, nonce, plain, cipher, tag);
		::SecureZeroMemory(plain.data(), plain.size());
		if (!ok)
		{
			return std::string();
		}

		AiJsonWriter w;
		w.BeginObject();
		w.StrUtf8("format", "ditto-sync");
		w.Int("version", PACKAGE_VERSION);
		w.StrUtf8("cipher", "AES-256-GCM");
		w.StrUtf8("kdf", "PBKDF2-SHA256");
		w.Int("iterations", key.iterations);
		w.StrUtf8("salt", Base64Encode(key.salt.data(), key.salt.size()));
		w.StrUtf8("nonce", Base64Encode(nonce.data(), nonce.size()));
		w.StrUtf8("tag", Base64Encode(tag.data(), tag.size()));
		w.StrUtf8("data", Base64Encode(cipher.data(), cipher.size()));
		w.EndObject();
		return w.Result();
	}

	// Keeps the derived key in 'key' so the answer can be encrypted again without a second PBKDF2 run.
	bool DecryptPackage(const std::string &body, const CString &password, CryptoKey &key, std::string &json, CString &error)
	{
		AiJson root;
		if (!AiJson::Parse(body, root) || root.type != AiJson::Object || root.GetString("format") != "ditto-sync")
		{
			error = _T("Buluttaki veri bir Ditto senkron paketi değil. API adresini kontrol edin.");
			return false;
		}
		if ((int)root.GetNumber("version", 0) > PACKAGE_VERSION)
		{
			error = _T("Buluttaki paket Ditto'nun daha yeni bir sürümüyle oluşturulmuş. Bu bilgisayardaki Ditto'yu güncelleyin.");
			return false;
		}

		Bytes salt, nonce, tag, cipher;
		ULONG iterations = (ULONG)root.GetNumber("iterations", 0);
		if (root.GetString("cipher") != "AES-256-GCM" || root.GetString("kdf") != "PBKDF2-SHA256" || iterations < 1000 ||
			!Base64Decode(root.GetString("salt"), salt) || !Base64Decode(root.GetString("nonce"), nonce) ||
			!Base64Decode(root.GetString("tag"), tag) || !Base64Decode(root.GetString("data"), cipher) ||
			salt.empty() || nonce.size() != 12 || tag.size() != 16)
		{
			error = _T("Buluttaki paketin şifreleme bilgileri okunamadı.");
			return false;
		}

		if (key.key.empty() || key.salt != salt || key.iterations != iterations)
		{
			key.salt = salt;
			key.iterations = iterations;
			if (!DeriveKey(password, key))
			{
				error = _T("Şifreleme anahtarı üretilemedi.");
				return false;
			}
		}

		Bytes plain;
		if (!AesGcm(false, key.key, nonce, cipher, plain, tag))
		{
			key.key.clear();
			error = _T("Buluttaki paket bu parolayla açılamadı. Şifreleme parolası tüm bilgisayarlarda aynı olmalı.");
			return false;
		}
		json.assign((const char *)plain.data(), plain.size());
		::SecureZeroMemory(plain.data(), plain.size());
		return true;
	}

	// ---------------------------------------------------------------- HTTP

	struct HttpResponse
	{
		bool transportOk = false;
		DWORD status = 0;
		std::string body;
		std::string etag;
		CString error;
	};

	CString WinHttpErrorText(DWORD error)
	{
		switch (error)
		{
		case ERROR_WINHTTP_TIMEOUT: return _T("Sunucu zaman aşımına uğradı.");
		case ERROR_WINHTTP_NAME_NOT_RESOLVED: return _T("Sunucu adı çözülemedi. API adresini ve internet bağlantısını kontrol edin.");
		case ERROR_WINHTTP_CANNOT_CONNECT:
		case ERROR_WINHTTP_CONNECTION_ERROR: return _T("Sunucuya bağlanılamadı. API adresini, sunucunun çalıştığını ve internet bağlantısını kontrol edin.");
		case ERROR_WINHTTP_SECURE_FAILURE: return _T("Güvenli bağlantı (TLS) kurulamadı.");
		case ERROR_WINHTTP_INVALID_URL:
		case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return _T("API adresi geçersiz.");
		default: return StrF(_T("Ağ hatası (WinHTTP %u)."), error);
		}
	}

	CString HttpErrorText(const HttpResponse &response)
	{
		if (!response.transportOk)
		{
			return response.error;
		}
		switch (response.status)
		{
		case 400: return _T("Sunucu isteği reddetti (HTTP 400).");
		case 401: return _T("API anahtarı geçersiz (HTTP 401).");
		case 403: return _T("Bu API anahtarının bu adrese erişim izni yok (HTTP 403).");
		case 404: return _T("Adres bulunamadı (HTTP 404).");
		case 405: return _T("Sunucu bu işlemi desteklemiyor (HTTP 405). API adresini kontrol edin.");
		case 413: return _T("Paket sunucunun kabul ettiği boyuttan büyük (HTTP 413). Resim/RTF/HTML gönderimini kapatın veya en büyük klip boyutunu küçültün.");
		case 429: return _T("Sunucu çok fazla istek aldı (HTTP 429). Biraz sonra tekrar denenecek.");
		default:
			if (response.status >= 500)
			{
				return StrF(_T("Sunucu şu anda yanıt veremiyor (HTTP %u)."), response.status);
			}
			return StrF(_T("Beklenmeyen sunucu yanıtı (HTTP %u)."), response.status);
		}
	}

	// extraHeaders: "Name: value\r\n" lines. body == nullptr: no request body.
	HttpResponse HttpRequest(LPCWSTR method, const CString &url, const CString &apiKey, const CString &extraHeaders, const std::string *body)
	{
		HttpResponse response;

		URL_COMPONENTS parts = { sizeof(URL_COMPONENTS) };
		wchar_t host[256] = { 0 };
		wchar_t path[2048] = { 0 };
		wchar_t extra[2048] = { 0 };
		parts.lpszHostName = host;
		parts.dwHostNameLength = _countof(host);
		parts.lpszUrlPath = path;
		parts.dwUrlPathLength = _countof(path);
		parts.lpszExtraInfo = extra;
		parts.dwExtraInfoLength = _countof(extra);

		CString trimmedUrl = url;
		trimmedUrl.Trim();
		if (!::WinHttpCrackUrl(trimmedUrl, 0, 0, &parts))
		{
			response.error = _T("API adresi geçersiz: ") + trimmedUrl;
			return response;
		}
		CString fullPath = CString(path) + CString(extra);
		bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

		HINTERNET hSession = ::WinHttpOpen(L"Ditto-Sync/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (hSession == NULL)
		{
			hSession = ::WinHttpOpen(L"Ditto-Sync/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		}

		HINTERNET hConnect = NULL, hRequest = NULL;
		if (hSession != NULL)
		{
			// resolve, connect, send, receive (ms)
			::WinHttpSetTimeouts(hSession, 10000, 10000, 60000, 120000);
			hConnect = ::WinHttpConnect(hSession, host, parts.nPort, 0);
		}
		if (hConnect != NULL)
		{
			hRequest = ::WinHttpOpenRequest(hConnect, method, fullPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
		}

		bool ok = false;
		if (hRequest != NULL)
		{
			CString headers = _T("Accept: application/json\r\n");
			if (apiKey.IsEmpty() == FALSE)
			{
				headers += _T("Authorization: Bearer ") + apiKey + _T("\r\n");
			}
			if (body != nullptr)
			{
				headers += _T("Content-Type: application/json\r\n");
			}
			headers += extraHeaders;

			LPVOID data = body != nullptr ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA;
			DWORD size = body != nullptr ? (DWORD)body->size() : 0;
			if (::WinHttpSendRequest(hRequest, headers, (DWORD)-1L, data, size, size, 0) &&
				::WinHttpReceiveResponse(hRequest, NULL))
			{
				DWORD statusSize = sizeof(response.status);
				::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusSize, WINHTTP_NO_HEADER_INDEX);

				DWORD etagBytes = 0;
				::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &etagBytes, WINHTTP_NO_HEADER_INDEX);
				if (::GetLastError() == ERROR_INSUFFICIENT_BUFFER && etagBytes > 0)
				{
					std::wstring etag(etagBytes / sizeof(wchar_t) + 1, L'\0');
					if (::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, &etag[0], &etagBytes, WINHTTP_NO_HEADER_INDEX))
					{
						etag.resize(etagBytes / sizeof(wchar_t));
						response.etag = AiToUtf8(CString(etag.c_str()));
					}
				}

				ok = true;
				while (true)
				{
					DWORD available = 0;
					if (!::WinHttpQueryDataAvailable(hRequest, &available))
					{
						ok = false;
						break;
					}
					if (available == 0)
					{
						break;
					}
					size_t offset = response.body.size();
					response.body.resize(offset + available);
					DWORD read = 0;
					if (!::WinHttpReadData(hRequest, &response.body[offset], available, &read))
					{
						ok = false;
						break;
					}
					response.body.resize(offset + read);
				}
			}
		}

		if (!ok)
		{
			response.error = WinHttpErrorText(::GetLastError());
		}
		response.transportOk = ok;

		if (hRequest) ::WinHttpCloseHandle(hRequest);
		if (hConnect) ::WinHttpCloseHandle(hConnect);
		if (hSession) ::WinHttpCloseHandle(hSession);

		return response;
	}

	// ---------------------------------------------------------------- package

	struct SyncFormat
	{
		CString name;			// Ditto's format name, e.g. CF_UNICODETEXT, Rich Text Format, PNG
		Bytes data;
	};

	struct SyncEntry
	{
		CString id;
		__int64 modified = 0;	// last change, decides which version wins
		bool isGroup = false;
		CString parent;			// id of the group, empty = top level
		__int64 date = 0;		// copy time
		__int64 lastPaste = 0;
		CString desc;
		CString quickPaste;
		bool neverDelete = false;
		bool sticky = false;
		std::string contentHash;
		std::vector<SyncFormat> formats;
	};

	struct SyncPackage
	{
		std::vector<SyncEntry> entries;
		std::map<CString, __int64> deleted;		// id -> deletion time
		__int64 updatedAt = 0;
		CString updatedBy;
	};

	bool IsUnicodeText(const CString &formatName)
	{
		return formatName == UNICODE_TEXT;
	}

	// CF_UNICODETEXT travels as text: keep "text up to the first NUL + NUL" so every computer has the same bytes.
	Bytes CanonicalUnicode(const BYTE *data, size_t size)
	{
		const wchar_t *text = (const wchar_t *)data;
		size_t chars = size / sizeof(wchar_t);
		size_t length = 0;
		while (length < chars && text[length] != 0)
		{
			length++;
		}
		Bytes out((length + 1) * sizeof(wchar_t), 0);
		memcpy(out.data(), data, length * sizeof(wchar_t));
		return out;
	}

	CString UnicodeBytesToText(const Bytes &data)
	{
		Bytes canonical = CanonicalUnicode(data.data(), data.size());
		return CString((const wchar_t *)canonical.data());
	}

	std::string ContentHash(const std::vector<SyncFormat> &formats)
	{
		if (formats.empty())
		{
			return std::string();
		}
		std::vector<const SyncFormat *> sorted;
		for (const SyncFormat &f : formats)
		{
			sorted.push_back(&f);
		}
		std::sort(sorted.begin(), sorted.end(), [](const SyncFormat *a, const SyncFormat *b) { return a->name < b->name; });

		Sha256 hash;
		for (const SyncFormat *f : sorted)
		{
			hash.AddField(f->name);
			hash.AddField(f->data.data(), f->data.size());
		}
		return hash.HexDigest();
	}

	std::string PropsHash(bool isGroup, const CString &parent, const CString &desc, const CString &quickPaste, bool neverDelete, bool sticky)
	{
		Sha256 hash;
		BYTE flags[3] = { (BYTE)isGroup, (BYTE)neverDelete, (BYTE)sticky };
		hash.AddField(flags, sizeof(flags));
		hash.AddField(parent);
		hash.AddField(desc);
		hash.AddField(quickPaste);
		return hash.HexDigest();
	}

	std::string EntryProps(const SyncEntry &e)
	{
		return PropsHash(e.isGroup, e.parent, e.desc, e.quickPaste, e.neverDelete, e.sticky);
	}

	// The same text copied on two computers becomes one clip, whatever extra formats each one kept.
	std::string MatchKey(const SyncEntry &e)
	{
		Sha256 hash;
		if (e.isGroup)
		{
			hash.AddField("group", 5);
			hash.AddField(e.desc);
			return hash.HexDigest();
		}
		for (const SyncFormat &f : e.formats)
		{
			if (IsUnicodeText(f.name))
			{
				hash.AddField("text", 4);
				hash.AddField(f.data.data(), f.data.size());
				return hash.HexDigest();
			}
		}
		return "content:" + e.contentHash;
	}

	std::string BuildPackageJson(const SyncPackage &package, const CString &device)
	{
		AiJsonWriter w;
		w.BeginObject();
		w.StrUtf8("format", "ditto-sync-data");
		w.Int("version", PACKAGE_VERSION);
		w.Int("updatedAt", Now());
		w.Str("updatedBy", device);

		w.BeginArray("clips");
		for (const SyncEntry &e : package.entries)
		{
			w.BeginObject();
			w.Str("id", e.id);
			w.Int("modified", e.modified);
			w.Bool("group", e.isGroup);
			w.Str("parent", e.parent);
			w.Int("date", e.date);
			w.Int("lastPaste", e.lastPaste);
			w.Str("desc", e.desc);
			w.Str("quickPaste", e.quickPaste);
			w.Bool("neverDelete", e.neverDelete);
			w.Bool("sticky", e.sticky);
			w.StrUtf8("contentHash", e.contentHash);
			w.BeginArray("formats");
			for (const SyncFormat &f : e.formats)
			{
				w.BeginObject();
				w.Str("name", f.name);
				if (IsUnicodeText(f.name))
				{
					w.Str("text", UnicodeBytesToText(f.data));
				}
				else
				{
					w.StrUtf8("base64", Base64Encode(f.data.data(), f.data.size()));
				}
				w.EndObject();
			}
			w.EndArray();
			w.EndObject();
		}
		w.EndArray();

		w.BeginArray("deleted");
		for (const auto &d : package.deleted)
		{
			w.BeginObject();
			w.Str("id", d.first);
			w.Int("at", d.second);
			w.EndObject();
		}
		w.EndArray();

		w.EndObject();
		return w.Result();
	}

	bool ParsePackageJson(const std::string &json, SyncPackage &package, CString &error)
	{
		AiJson root;
		if (!AiJson::Parse(json, root) || root.type != AiJson::Object || root.GetString("format") != "ditto-sync-data")
		{
			error = _T("Buluttaki paketin içeriği okunamadı.");
			return false;
		}
		if ((int)root.GetNumber("version", 0) > PACKAGE_VERSION)
		{
			error = _T("Buluttaki paket Ditto'nun daha yeni bir sürümüyle oluşturulmuş. Bu bilgisayardaki Ditto'yu güncelleyin.");
			return false;
		}

		package.updatedAt = (__int64)root.GetNumber("updatedAt", 0);
		package.updatedBy = root.GetCString("updatedBy");

		for (const AiJson &jc : root.GetArray("clips"))
		{
			SyncEntry e;
			e.id = jc.GetCString("id");
			if (e.id.IsEmpty())
			{
				continue;
			}
			e.modified = (__int64)jc.GetNumber("modified", 0);
			e.isGroup = jc.GetBool("group", false);
			e.parent = jc.GetCString("parent");
			e.date = (__int64)jc.GetNumber("date", 0);
			e.lastPaste = (__int64)jc.GetNumber("lastPaste", 0);
			e.desc = jc.GetCString("desc");
			e.quickPaste = jc.GetCString("quickPaste");
			e.neverDelete = jc.GetBool("neverDelete", false);
			e.sticky = jc.GetBool("sticky", false);
			e.contentHash = jc.GetString("contentHash");

			for (const AiJson &jf : jc.GetArray("formats"))
			{
				SyncFormat f;
				f.name = jf.GetCString("name");
				if (f.name.IsEmpty())
				{
					continue;
				}
				if (IsUnicodeText(f.name))
				{
					CString text = jf.GetCString("text");
					const BYTE *p = (const BYTE *)(LPCWSTR)text;
					f.data.assign(p, p + (text.GetLength() + 1) * sizeof(wchar_t));
				}
				else if (!Base64Decode(jf.GetString("base64"), f.data))
				{
					continue;
				}
				e.formats.push_back(std::move(f));
			}
			package.entries.push_back(std::move(e));
		}

		for (const AiJson &jd : root.GetArray("deleted"))
		{
			CString id = jd.GetCString("id");
			if (id.IsEmpty() == FALSE)
			{
				package.deleted[id] = (__int64)jd.GetNumber("at", 0);
			}
		}
		return true;
	}

	// ---------------------------------------------------------------- local database

	struct LocalClip
	{
		int id = 0;
		bool isGroup = false;
		int parentId = -1;		// <= 0: top level
		CString desc;
		CString quickPaste;
		bool neverDelete = false;
		bool sticky = false;
		int crc = 0;
		__int64 date = 0;
		__int64 lastPaste = 0;
	};

	// One row of the DittoSync table: what this computer and the cloud agreed on at the last sync.
	struct SyncRow
	{
		CString guid;
		int clipId = 0;
		int crc = 0;				// Main.CRC at the last sync
		std::string propsHash;
		std::string contentHash;
		__int64 modified = 0;		// time of the version both sides have, or of the local change when dirty
		bool dirty = false;			// changed here, not uploaded yet
		__int64 deletedAt = 0;		// deleted here

		bool changed = false;		// in memory: needs saving
		bool linked = false;		// in memory: new here, same text as a cloud clip, take the cloud's properties
	};

	class WriteBatch
	{
	public:
		explicit WriteBatch(CppSQLite3DB &db) : m_db(db) {}
		~WriteBatch()
		{
			if (m_open)
			{
				try { m_db.execDML(_T("ROLLBACK")); }
				catch (CppSQLite3Exception &) {}
			}
		}

		void Begin()
		{
			if (!m_open)
			{
				m_db.execDML(_T("BEGIN IMMEDIATE"));
				m_open = true;
			}
		}

		// short transactions so Ditto itself never waits long for the database
		void Done()
		{
			if (++m_count >= 100)
			{
				Commit();
			}
		}

		void Commit()
		{
			if (m_open)
			{
				m_db.execDML(_T("COMMIT"));
				m_open = false;
			}
			m_count = 0;
		}

	private:
		CppSQLite3DB &m_db;
		bool m_open = false;
		int m_count = 0;
	};

	std::map<int, LocalClip> LoadLocalClips(CppSQLite3DB &db)
	{
		std::map<int, LocalClip> clips;
		CppSQLite3Query q = db.execQuery(_T("SELECT lID, bIsGroup, lParentID, mText, QuickPasteText, lDontAutoDelete, CRC, lDate, lastPasteDate, stickyClipOrder FROM Main"));
		for (; !q.eof(); q.nextRow())
		{
			LocalClip c;
			c.id = q.getIntField(0);
			c.isGroup = q.getIntField(1) != 0;
			c.parentId = q.getIntField(2, -1);
			c.desc = q.getStringField(3);
			c.quickPaste = q.getStringField(4);
			c.neverDelete = q.getIntField(5) != 0;
			c.crc = q.getIntField(6);
			c.date = q.getInt64Field(7);
			c.lastPaste = q.getInt64Field(8);
			c.sticky = q.getFloatField(9, INVALID_STICKY) != INVALID_STICKY;
			clips[c.id] = c;
		}
		return clips;
	}

	std::map<CString, SyncRow> LoadRows(CppSQLite3DB &db)
	{
		std::map<CString, SyncRow> rows;
		CppSQLite3Query q = db.execQuery(_T("SELECT guid, clipID, crc, propsHash, contentHash, modified, dirty, deletedAt FROM DittoSync"));
		for (; !q.eof(); q.nextRow())
		{
			SyncRow r;
			r.guid = q.getStringField(0);
			r.clipId = q.getIntField(1);
			r.crc = q.getIntField(2);
			r.propsHash = AiToUtf8(q.getStringField(3));
			r.contentHash = AiToUtf8(q.getStringField(4));
			r.modified = q.getInt64Field(5);
			r.dirty = q.getIntField(6) != 0;
			r.deletedAt = q.getInt64Field(7);
			rows[r.guid] = r;
		}
		return rows;
	}

	void SaveRow(CppSQLite3DB &db, const SyncRow &r)
	{
		CppSQLite3Statement st = db.compileStatement(_T("INSERT OR REPLACE INTO DittoSync (guid, clipID, crc, propsHash, contentHash, modified, dirty, deletedAt) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
		CString propsHash(r.propsHash.c_str());
		CString contentHash(r.contentHash.c_str());
		st.bind(1, (LPCTSTR)r.guid);
		st.bind(2, r.clipId);
		st.bind(3, r.crc);
		st.bind(4, (LPCTSTR)propsHash);
		st.bind(5, (LPCTSTR)contentHash);
		st.bind(6, (double)r.modified);
		st.bind(7, r.dirty ? 1 : 0);
		st.bind(8, (double)r.deletedAt);
		st.execDML();
	}

	void DeleteRow(CppSQLite3DB &db, const CString &guid)
	{
		CppSQLite3Statement st = db.compileStatement(_T("DELETE FROM DittoSync WHERE guid = ?"));
		st.bind(1, (LPCTSTR)guid);
		st.execDML();
	}

	bool AllowedFormat(const CString &name, bool hasUnicodeText, bool includeRich)
	{
		if (name == UNICODE_TEXT)
		{
			return true;
		}
		if (name == _T("CF_TEXT"))
		{
			return includeRich || !hasUnicodeText;
		}
		return includeRich &&
			(name == _T("Rich Text Format") || name == _T("HTML Format") || name == _T("CF_DIB") || name == _T("PNG"));
	}

	std::vector<SyncFormat> LoadLocalFormats(CppSQLite3DB &db, int clipId, const DbSyncSettings &settings)
	{
		struct Meta { int dataId; CString name; int size; };
		std::vector<Meta> metas;
		bool hasUnicodeText = false;
		{
			// length() does not read the blob
			CppSQLite3Query q = db.execQueryEx(_T("SELECT lID, strClipBoardFormat, length(ooData) FROM Data WHERE lParentID = %d ORDER BY lID"), clipId);
			for (; !q.eof(); q.nextRow())
			{
				Meta m = { q.getIntField(0), q.getStringField(1), q.getIntField(2) };
				hasUnicodeText = hasUnicodeText || m.name == UNICODE_TEXT;
				metas.push_back(m);
			}
		}

		std::vector<SyncFormat> formats;
		int maxBytes = settings.maxClipKB * 1024;
		for (const Meta &m : metas)
		{
			if (!AllowedFormat(m.name, hasUnicodeText, settings.includeRichFormats) || m.size <= 0 || m.size > maxBytes)
			{
				continue;
			}
			CppSQLite3Query q = db.execQueryEx(_T("SELECT ooData FROM Data WHERE lID = %d"), m.dataId);
			if (q.eof())
			{
				continue;
			}
			int length = 0;
			const unsigned char *data = q.getBlobField(0, length);
			if (data == NULL || length <= 0)
			{
				continue;
			}
			SyncFormat f;
			f.name = m.name;
			if (IsUnicodeText(m.name))
			{
				f.data = CanonicalUnicode(data, length);
			}
			else
			{
				f.data.assign(data, data + length);
			}
			formats.push_back(std::move(f));
		}
		return formats;
	}

	// Order value that puts a clip copied at 'date' between the local clips copied before and after it.
	double PlaceOrder(CppSQLite3DB &db, int groupId, __int64 date)
	{
		CString column = groupId > 0 ? _T("clipGroupOrder") : _T("clipOrder");
		CString where = groupId > 0 ? StrF(_T("lParentID = %d"), groupId) : CString(_T("1 = 1"));

		CString sql;
		sql.Format(_T("SELECT %s FROM Main WHERE %s AND lDate <= %lld ORDER BY lDate DESC, %s DESC LIMIT 1"), (LPCTSTR)column, (LPCTSTR)where, date, (LPCTSTR)column);
		CppSQLite3Query lower = db.execQuery(sql);
		if (!lower.eof())
		{
			double lowerOrder = lower.getFloatField(0);
			sql.Format(_T("SELECT MIN(%s) FROM Main WHERE %s AND %s > %.17g"), (LPCTSTR)column, (LPCTSTR)where, (LPCTSTR)column, lowerOrder);
			CppSQLite3Query upper = db.execQuery(sql);
			if (!upper.eof() && !upper.fieldIsNull(0))
			{
				return (lowerOrder + upper.getFloatField(0)) / 2.0;
			}
			return lowerOrder + 1;
		}

		sql.Format(_T("SELECT MIN(%s) FROM Main WHERE %s"), (LPCTSTR)column, (LPCTSTR)where);
		CppSQLite3Query first = db.execQuery(sql);
		if (!first.eof() && !first.fieldIsNull(0))
		{
			return first.getFloatField(0) - 1;
		}
		return 1;
	}

	double TopStickyOrder(CppSQLite3DB &db)
	{
		CppSQLite3Query q = db.execQuery(_T("SELECT MAX(stickyClipOrder) FROM Main WHERE stickyClipOrder <> -(2147483647)"));
		if (!q.eof() && !q.fieldIsNull(0))
		{
			return q.getFloatField(0) + 1;
		}
		return 1;
	}

	// Ditto's own CRC over the formats, so duplicate detection keeps working for synced clips.
	int ClipCrc(const std::vector<SyncFormat> &formats)
	{
		CClip clip;
		for (const SyncFormat &f : formats)
		{
			CLIPFORMAT cf = GetFormatID(f.name);
			if (cf != 0 && f.data.empty() == false)
			{
				clip.AddFormat(cf, (void *)f.data.data(), (UINT)f.data.size());
			}
		}
		return (int)clip.GenerateCRC();
	}

	void InsertFormats(CppSQLite3DB &db, int clipId, const std::vector<SyncFormat> &formats)
	{
		CppSQLite3Statement st = db.compileStatement(_T("INSERT INTO Data (lID, lParentID, strClipBoardFormat, ooData) VALUES (NULL, ?, ?, ?)"));
		for (const SyncFormat &f : formats)
		{
			if (f.data.empty())
			{
				continue;
			}
			st.bind(1, clipId);
			st.bind(2, (LPCTSTR)f.name);
			st.bind(3, f.data.data(), (int)f.data.size());
			st.execDML();
			st.reset();
		}
	}

	void InsertLocal(CppSQLite3DB &db, const SyncEntry &e, int parentId, int &newId, int &crc)
	{
		__int64 date = e.date > 0 ? e.date : Now();
		crc = e.isGroup ? 0 : ClipCrc(e.formats);
		double order = PlaceOrder(db, -1, date);
		double groupOrder = parentId > 0 ? PlaceOrder(db, parentId, date) : 0;
		double sticky = e.sticky ? TopStickyOrder(db) : (double)INVALID_STICKY;

		CString sql;
		sql.Format(_T("INSERT INTO Main (lDate, mText, lShortCut, lDontAutoDelete, CRC, bIsGroup, lParentID, QuickPasteText, clipOrder, clipGroupOrder, ")
			_T("globalShortCut, lastPasteDate, stickyClipOrder, stickyClipGroupOrder, MoveToGroupShortCut, GlobalMoveToGroupShortCut) ")
			_T("VALUES (%lld, ?, 0, %d, %d, %d, %d, ?, %.17g, %.17g, 0, %lld, %.17g, %d, 0, 0)"),
			date, e.neverDelete ? (int)Now() : 0, crc, e.isGroup ? 1 : 0, parentId, order, groupOrder,
			e.lastPaste > 0 ? e.lastPaste : date, sticky, INVALID_STICKY);
		CppSQLite3Statement st = db.compileStatement(sql);
		st.bind(1, (LPCTSTR)e.desc);
		st.bind(2, (LPCTSTR)e.quickPaste);
		st.execDML();
		newId = (int)db.lastRowId();

		if (!e.isGroup)
		{
			InsertFormats(db, newId, e.formats);
		}
	}

	// Writes the cloud version over a local clip. Returns the new CRC.
	int ApplyToLocal(CppSQLite3DB &db, const LocalClip &c, const SyncEntry &e, int parentId, bool replaceContent)
	{
		CString extra;
		if (e.neverDelete != c.neverDelete)
		{
			extra += StrF(_T(", lDontAutoDelete = %d"), e.neverDelete ? (int)Now() : 0);
		}
		int localParent = c.parentId > 0 ? c.parentId : -1;
		if (parentId != localParent && parentId > 0)
		{
			extra += StrF(_T(", clipGroupOrder = %.17g"), PlaceOrder(db, parentId, c.date));
		}
		if (e.sticky != c.sticky)
		{
			extra += StrF(_T(", stickyClipOrder = %.17g"), e.sticky ? TopStickyOrder(db) : (double)INVALID_STICKY);
		}

		int crc = c.crc;
		if (replaceContent && !c.isGroup)
		{
			db.execDMLEx(_T("DELETE FROM Data WHERE lParentID = %d"), c.id);
			InsertFormats(db, c.id, e.formats);
			crc = ClipCrc(e.formats);
			extra += StrF(_T(", CRC = %d"), crc);
		}

		CString sql = _T("UPDATE Main SET mText = ?, QuickPasteText = ?, lParentID = ?") + extra + StrF(_T(" WHERE lID = %d"), c.id);
		CppSQLite3Statement st = db.compileStatement(sql);
		st.bind(1, (LPCTSTR)e.desc);
		st.bind(2, (LPCTSTR)e.quickPaste);
		st.bind(3, parentId);
		st.execDML();
		return crc;
	}

	struct TestParams
	{
		DbSyncSettings settings;
		HWND hNotify;
	};

	DWORD WINAPI TestThreadProc(LPVOID param)
	{
		std::unique_ptr<TestParams> params((TestParams *)param);
		CString *pMessage = new CString();
		bool ok = DbSyncTestConnection(params->settings, *pMessage);
		if (!::IsWindow(params->hNotify) || !::PostMessage(params->hNotify, WM_DB_SYNC_TEST_DONE, ok ? 1 : 0, (LPARAM)pMessage))
		{
			delete pMessage;
		}
		return 0;
	}
}

// ===========================================================================
// Settings

void DbSyncSettings::Load()
{
	enabled = CGetSetOptions::GetProfileLong(_T("DbSyncEnabled"), 0) != 0;
	url = CGetSetOptions::GetProfileString(_T("DbSyncUrl"), _T(""));
	apiKey = AiUnprotectSecret(CGetSetOptions::GetProfileString(_T("DbSyncApiKey"), _T("")));
	password = AiUnprotectSecret(CGetSetOptions::GetProfileString(_T("DbSyncPassword"), _T("")));
	autoSync = CGetSetOptions::GetProfileLong(_T("DbSyncAuto"), 1) != 0;
	intervalMinutes = max(1, min(1440, (int)CGetSetOptions::GetProfileLong(_T("DbSyncIntervalMinutes"), 15)));
	syncOnStart = CGetSetOptions::GetProfileLong(_T("DbSyncOnStart"), 1) != 0;
	includeRichFormats = CGetSetOptions::GetProfileLong(_T("DbSyncRichFormats"), 0) != 0;
	maxClipKB = max(1, min(102400, (int)CGetSetOptions::GetProfileLong(_T("DbSyncMaxClipKB"), 1024)));
	propagateDeletes = CGetSetOptions::GetProfileLong(_T("DbSyncPropagateDeletes"), 0) != 0;
}

void DbSyncSettings::Save() const
{
	CGetSetOptions::SetProfileLong(_T("DbSyncEnabled"), enabled ? 1 : 0);
	CGetSetOptions::SetProfileString(_T("DbSyncUrl"), url);
	CGetSetOptions::SetProfileString(_T("DbSyncApiKey"), AiProtectSecret(apiKey));
	CGetSetOptions::SetProfileString(_T("DbSyncPassword"), AiProtectSecret(password));
	CGetSetOptions::SetProfileLong(_T("DbSyncAuto"), autoSync ? 1 : 0);
	CGetSetOptions::SetProfileLong(_T("DbSyncIntervalMinutes"), intervalMinutes);
	CGetSetOptions::SetProfileLong(_T("DbSyncOnStart"), syncOnStart ? 1 : 0);
	CGetSetOptions::SetProfileLong(_T("DbSyncRichFormats"), includeRichFormats ? 1 : 0);
	CGetSetOptions::SetProfileLong(_T("DbSyncMaxClipKB"), maxClipKB);
	CGetSetOptions::SetProfileLong(_T("DbSyncPropagateDeletes"), propagateDeletes ? 1 : 0);
}

CString DbSyncSettings::Validate() const
{
	CString trimmed = url;
	trimmed.Trim();
	if (trimmed.IsEmpty())
	{
		return _T("API adresini girin.");
	}
	if (trimmed.Left(8).CompareNoCase(_T("https://")) != 0 && trimmed.Left(7).CompareNoCase(_T("http://")) != 0)
	{
		return _T("API adresi http:// veya https:// ile başlamalı.");
	}
	if (password.GetLength() < 8)
	{
		return _T("Şifreleme parolası en az 8 karakter olmalı. Tüm bilgisayarlarda aynı parola kullanılır.");
	}
	return CString();
}

CString DbSyncResult::Summary() const
{
	CString when = CTime::GetCurrentTime().Format(_T("%d.%m.%Y %H:%M"));
	if (!ok)
	{
		return _T("Son deneme: ") + when + _T("\r\nHata: ") + error;
	}
	CString text;
	text.Format(_T("Son senkron: %s\r\nGönderilen %d · Alınan %d · Güncellenen %d · Silinen %d · Buluttaki klip %d"),
		(LPCTSTR)when, uploaded, downloaded, updated, deletedLocal + deletedRemote, remoteClips);
	return text;
}

// ===========================================================================
// Connection test

bool DbSyncTestConnection(const DbSyncSettings &settings, CString &message)
{
	message = settings.Validate();
	if (message.IsEmpty() == FALSE)
	{
		return false;
	}

	HttpResponse get = HttpRequest(L"GET", settings.url, settings.apiKey, CString(), nullptr);
	if (get.transportOk && get.status == 404)
	{
		message = _T("Bağlantı başarılı. Bulutta henüz paket yok; ilk senkronda oluşturulur.");
		return true;
	}
	if (!get.transportOk || get.status != 200)
	{
		message = HttpErrorText(get);
		return false;
	}

	CryptoKey key;
	std::string json;
	SyncPackage package;
	CString error;
	if (!DecryptPackage(get.body, settings.password, key, json, error) || !ParsePackageJson(json, package, error))
	{
		message = _T("Sunucuya bağlanıldı.\r\n") + error;
		return false;
	}

	message.Format(_T("Bağlantı ve parola doğru.\r\nBuluttaki paket: %d klip, son güncelleme %s (%s)."),
		(int)package.entries.size(), (LPCTSTR)FormatTime(package.updatedAt), (LPCTSTR)package.updatedBy);
	if (get.etag.empty())
	{
		message += _T("\r\nUyarı: Sunucu ETag başlığı döndürmüyor; senkron bu sunucuyla çalışmaz.");
		return false;
	}
	return true;
}

// ===========================================================================
// Sync

DbSyncResult DbSyncRun(const DbSyncSettings &settings, const CString &dbPath, volatile LONG *pStop, DbSyncSkipCache &skip)
{
	DbSyncResult result;
	result.error = settings.Validate();
	if (result.error.IsEmpty() == FALSE)
	{
		return result;
	}
	if (dbPath.IsEmpty())
	{
		result.error = _T("Veritabanı yolu boş.");
		return result;
	}

	if (skip.includeRich != settings.includeRichFormats || skip.maxClipKB != settings.maxClipKB)
	{
		skip.clips.clear();
		skip.includeRich = settings.includeRichFormats;
		skip.maxClipKB = settings.maxClipKB;
	}

	const CString stopped = _T("Ditto kapanırken senkron durduruldu.");
	CString device = ComputerName();
	CryptoKey key;

	try
	{
		// own connection: transactions here must not mix with Ditto's statements
		CppSQLite3DB db;
		db.open(dbPath);
		db.setBusyTimeout(10000);
		db.execDML(_T("CREATE TABLE IF NOT EXISTS DittoSync (guid TEXT PRIMARY KEY, clipID INTEGER, crc INTEGER, propsHash TEXT, ")
			_T("contentHash TEXT, modified INTEGER, dirty INTEGER, deletedAt INTEGER)"));

		for (int attempt = 1; ; attempt++)
		{
			if (*pStop)
			{
				result.error = stopped;
				return result;
			}

			// ---- cloud
			HttpResponse get = HttpRequest(L"GET", settings.url, settings.apiKey, CString(), nullptr);
			SyncPackage remote;
			bool remoteExists = false;
			if (get.transportOk && get.status == 200)
			{
				std::string json;
				if (!DecryptPackage(get.body, settings.password, key, json, result.error) || !ParsePackageJson(json, remote, result.error))
				{
					return result;
				}
				if (get.etag.empty())
				{
					result.error = _T("Sunucu ETag başlığı döndürmüyor. Aynı anda senkron yapan bilgisayarlar birbirinin verisini ezebileceği için senkron yapılmadı.");
					return result;
				}
				remoteExists = true;
			}
			else if (!get.transportOk || get.status != 404)
			{
				result.error = HttpErrorText(get);
				return result;
			}
			std::string().swap(get.body);

			std::map<CString, size_t> remoteIndex;
			for (size_t i = 0; i < remote.entries.size(); i++)
			{
				remoteIndex[remote.entries[i].id] = i;
			}

			// ---- this computer
			std::map<int, LocalClip> locals = LoadLocalClips(db);
			std::map<CString, SyncRow> rows = LoadRows(db);
			std::map<int, CString> guidOfClip;
			for (const auto &kv : rows)
			{
				if (kv.second.deletedAt == 0)
				{
					guidOfClip[kv.second.clipId] = kv.first;
				}
			}

			auto parentGuidOf = [&guidOfClip](const LocalClip &c) -> CString
			{
				if (c.parentId <= 0)
				{
					return CString();
				}
				auto it = guidOfClip.find(c.parentId);
				return it != guidOfClip.end() ? it->second : CString();
			};
			auto localProps = [&parentGuidOf](const LocalClip &c)
			{
				return PropsHash(c.isGroup, parentGuidOf(c), c.desc, c.quickPaste, c.neverDelete, c.sticky);
			};
			auto localIdOf = [&rows](const CString &guid) -> int
			{
				if (guid.IsEmpty())
				{
					return -1;
				}
				auto it = rows.find(guid);
				return (it != rows.end() && it->second.deletedAt == 0) ? it->second.clipId : -1;
			};

			__int64 now = Now();

			// 1. clips deleted here since the last sync
			for (auto &kv : rows)
			{
				SyncRow &r = kv.second;
				if (r.deletedAt == 0 && locals.find(r.clipId) == locals.end())
				{
					r.deletedAt = now;
					r.changed = true;
					guidOfClip.erase(r.clipId);
				}
			}

			// 2. clips new here: the same text as a cloud clip makes them the same clip, otherwise they get a new id
			std::map<std::string, CString> remoteByMatch;
			for (const SyncEntry &e : remote.entries)
			{
				if (rows.find(e.id) == rows.end())
				{
					remoteByMatch.emplace(MatchKey(e), e.id);
				}
			}
			std::map<CString, std::vector<SyncFormat>> newFormats;
			for (int pass = 0; pass < 2; pass++)		// groups first, their ids are needed for the clips' parents
			{
				for (const auto &kv : locals)
				{
					const LocalClip &c = kv.second;
					if (c.isGroup != (pass == 0) || guidOfClip.find(c.id) != guidOfClip.end())
					{
						continue;
					}

					SyncEntry probe;
					probe.isGroup = c.isGroup;
					probe.desc = c.desc;
					if (!c.isGroup)
					{
						if (skip.clips.count(std::make_pair(c.id, c.crc)) > 0)
						{
							continue;
						}
						probe.formats = LoadLocalFormats(db, c.id, settings);
						if (probe.formats.empty())
						{
							skip.clips.insert(std::make_pair(c.id, c.crc));
							continue;
						}
						probe.contentHash = ContentHash(probe.formats);
					}

					SyncRow r;
					r.clipId = c.id;
					r.crc = c.crc;
					r.changed = true;
					auto match = remoteByMatch.find(MatchKey(probe));
					if (match != remoteByMatch.end())
					{
						const SyncEntry &re = remote.entries[remoteIndex[match->second]];
						r.guid = re.id;
						r.propsHash = EntryProps(re);
						r.contentHash = re.contentHash;
						r.modified = re.modified;
						r.linked = true;
						remoteByMatch.erase(match);
					}
					else
					{
						r.guid = NewId();
						r.modified = c.date > 0 ? c.date : now;
						r.dirty = true;
						if (!c.isGroup)
						{
							newFormats[r.guid] = std::move(probe.formats);
						}
					}
					guidOfClip[c.id] = r.guid;
					rows[r.guid] = r;
				}
			}

			// 3. clips changed here since the last sync
			for (auto &kv : rows)
			{
				SyncRow &r = kv.second;
				if (r.deletedAt != 0 || r.linked)
				{
					continue;
				}
				const LocalClip &c = locals[r.clipId];
				bool changed = (!c.isGroup && c.crc != r.crc) || localProps(c) != r.propsHash;
				if (changed && !r.dirty)
				{
					r.dirty = true;
					r.modified = now;
					r.changed = true;
				}
			}

			// 4. merge: per clip the newest change wins
			std::vector<CString> downloads;			// cloud clips to add here
			std::vector<CString> applies;			// local clips to overwrite with the cloud version
			std::vector<CString> localDeletes;		// local clips deleted on another computer
			std::map<CString, SyncEntry> uploads;	// new or changed here
			std::set<CString> removeRemote;			// deleted here, removed from the cloud
			std::set<CString> removeTombstones;		// deleted elsewhere but changed here later
			std::set<CString> dropRows;
			std::map<CString, __int64> newTombstones;

			auto addUpload = [&](const SyncRow &r, const LocalClip &c, const SyncEntry *re)
			{
				SyncEntry e;
				e.id = r.guid;
				e.modified = r.modified;
				e.isGroup = c.isGroup;
				e.parent = parentGuidOf(c);
				e.date = c.date;
				e.lastPaste = c.lastPaste;
				e.desc = c.desc;
				e.quickPaste = c.quickPaste;
				e.neverDelete = c.neverDelete;
				e.sticky = c.sticky;
				if (!c.isGroup)
				{
					auto nf = newFormats.find(r.guid);
					if (nf != newFormats.end())
					{
						e.formats = std::move(nf->second);
					}
					else if (re != nullptr && c.crc == r.crc)
					{
						e.formats = re->formats;	// content unchanged: keep what the cloud has, also formats this computer does not send
					}
					else
					{
						e.formats = LoadLocalFormats(db, c.id, settings);
					}
					if (e.formats.empty())
					{
						return false;
					}
					e.contentHash = ContentHash(e.formats);
				}
				uploads[r.guid] = std::move(e);
				return true;
			};

			std::set<CString> ids;
			for (const auto &kv : rows)
			{
				ids.insert(kv.first);
			}
			for (const SyncEntry &e : remote.entries)
			{
				ids.insert(e.id);
			}

			for (const CString &id : ids)
			{
				auto rowIt = rows.find(id);
				SyncRow *r = rowIt != rows.end() ? &rowIt->second : nullptr;
				auto ri = remoteIndex.find(id);
				const SyncEntry *re = ri != remoteIndex.end() ? &remote.entries[ri->second] : nullptr;
				auto ti = remote.deleted.find(id);
				__int64 tombstone = ti != remote.deleted.end() ? ti->second : 0;

				if (r != nullptr && r->deletedAt == 0)
				{
					const LocalClip &c = locals[r->clipId];
					if (re != nullptr)
					{
						if (r->linked)
						{
							applies.push_back(id);
						}
						else if (r->dirty && r->modified >= re->modified)
						{
							addUpload(*r, c, re);
						}
						else if (r->dirty || re->modified != r->modified || EntryProps(*re) != r->propsHash || re->contentHash != r->contentHash)
						{
							applies.push_back(id);
						}
					}
					else if (ti != remote.deleted.end())
					{
						if (r->dirty && r->modified > tombstone)
						{
							if (addUpload(*r, c, nullptr))
							{
								removeTombstones.insert(id);
							}
						}
						else
						{
							localDeletes.push_back(id);
						}
					}
					else
					{
						addUpload(*r, c, nullptr);
					}
				}
				else if (r != nullptr)
				{
					// deleted here
					if (re == nullptr)
					{
						dropRows.insert(id);
					}
					else if (re->modified > r->deletedAt)
					{
						downloads.push_back(id);	// changed elsewhere after it was deleted here
					}
					else if (settings.propagateDeletes)
					{
						removeRemote.insert(id);
						newTombstones[id] = r->deletedAt;
					}
					// else: stays in the cloud, the row keeps it from coming back here
				}
				else if (re != nullptr)
				{
					downloads.push_back(id);
				}
			}

			// ---- write the local side
			{
				WriteBatch batch(db);

				for (auto &kv : rows)
				{
					if (kv.second.changed)
					{
						batch.Begin();
						SaveRow(db, kv.second);
						batch.Done();
					}
				}
				for (const CString &id : dropRows)
				{
					batch.Begin();
					DeleteRow(db, id);
					batch.Done();
					rows.erase(id);
				}

				// parents before children
				std::map<CString, int> depth;
				for (const CString &id : downloads)
				{
					int d = 0;
					CString parent = remote.entries[remoteIndex[id]].parent;
					while (parent.IsEmpty() == FALSE && d < 32)
					{
						auto it = remoteIndex.find(parent);
						if (it == remoteIndex.end())
						{
							break;
						}
						parent = remote.entries[it->second].parent;
						d++;
					}
					depth[id] = d;
				}
				std::stable_sort(downloads.begin(), downloads.end(), [&](const CString &a, const CString &b)
				{
					bool groupA = remote.entries[remoteIndex[a]].isGroup;
					bool groupB = remote.entries[remoteIndex[b]].isGroup;
					if (groupA != groupB)
					{
						return groupA;
					}
					return depth[a] < depth[b];
				});

				for (const CString &id : downloads)
				{
					if (*pStop)
					{
						batch.Commit();
						result.error = stopped;
						return result;
					}
					const SyncEntry &re = remote.entries[remoteIndex[id]];
					int newId = 0, crc = 0;
					batch.Begin();
					InsertLocal(db, re, localIdOf(re.parent), newId, crc);
					SyncRow r;
					r.guid = id;
					r.clipId = newId;
					r.crc = crc;
					r.propsHash = EntryProps(re);
					r.contentHash = re.contentHash;
					r.modified = re.modified;
					SaveRow(db, r);
					batch.Done();
					rows[id] = r;
					guidOfClip[newId] = id;
					result.downloaded++;
					result.localChanged = true;
				}

				for (const CString &id : applies)
				{
					SyncRow &r = rows[id];
					const LocalClip &c = locals[r.clipId];
					const SyncEntry &re = remote.entries[remoteIndex[id]];
					bool replaceContent = !c.isGroup && !r.linked && (re.contentHash != r.contentHash || c.crc != r.crc);
					bool propsDiffer = EntryProps(re) != localProps(c);

					batch.Begin();
					int crc = c.crc;
					if (propsDiffer || replaceContent)
					{
						crc = ApplyToLocal(db, c, re, localIdOf(re.parent), replaceContent);
						result.updated++;
						result.localChanged = true;
					}
					r.crc = crc;
					r.propsHash = EntryProps(re);
					r.contentHash = re.contentHash;
					r.modified = re.modified;
					r.dirty = false;
					SaveRow(db, r);
					batch.Done();
				}

				for (const CString &id : localDeletes)
				{
					SyncRow &r = rows[id];
					batch.Begin();
					db.execDMLEx(_T("DELETE FROM Main WHERE lID = %d"), r.clipId);
					DeleteRow(db, id);
					batch.Done();
					guidOfClip.erase(r.clipId);
					rows.erase(id);
					result.deletedLocal++;
					result.localChanged = true;
				}

				batch.Commit();
			}

			// ---- upload when the cloud is behind
			std::map<CString, __int64> deleted;
			bool pruned = false;
			for (const auto &kv : remote.deleted)
			{
				if (removeTombstones.count(kv.first) > 0)
				{
					continue;
				}
				if (kv.second < now - TOMBSTONE_KEEP_SECONDS)
				{
					pruned = true;
					continue;
				}
				deleted.insert(kv);
			}
			for (const auto &kv : newTombstones)
			{
				deleted[kv.first] = kv.second;
			}

			if (uploads.empty() && removeRemote.empty() && newTombstones.empty() && removeTombstones.empty() && !pruned)
			{
				result.remoteClips = (int)remote.entries.size();
				result.ok = true;
				return result;
			}

			struct Mark { CString guid; int crc; std::string props; std::string content; __int64 modified; };
			std::vector<Mark> marks;
			for (const auto &kv : uploads)
			{
				const SyncEntry &e = kv.second;
				marks.push_back({ kv.first, locals[rows[kv.first].clipId].crc, EntryProps(e), e.contentHash, e.modified });
			}
			int uploadCount = (int)uploads.size();

			SyncPackage out;
			out.entries.reserve(remote.entries.size() + uploads.size());
			for (SyncEntry &e : remote.entries)
			{
				if (removeRemote.count(e.id) > 0)
				{
					continue;
				}
				auto u = uploads.find(e.id);
				if (u != uploads.end())
				{
					out.entries.push_back(std::move(u->second));
					uploads.erase(u);
				}
				else
				{
					out.entries.push_back(std::move(e));
				}
			}
			for (auto &kv : uploads)
			{
				out.entries.push_back(std::move(kv.second));
			}
			out.deleted = std::move(deleted);

			std::string body = EncryptPackage(BuildPackageJson(out, device), settings.password, key);
			if (body.empty())
			{
				result.error = _T("Paket şifrelenemedi.");
				return result;
			}

			CString condition = remoteExists ? _T("If-Match: ") + AiFromUtf8(get.etag) + _T("\r\n") : CString(_T("If-None-Match: *\r\n"));
			HttpResponse put = HttpRequest(L"PUT", settings.url, settings.apiKey, condition, &body);
			if (put.transportOk && (put.status == 412 || put.status == 409))
			{
				if (attempt < MAX_ATTEMPTS)
				{
					continue;	// another computer uploaded first: merge with its version
				}
				result.error = _T("Başka bir bilgisayar aynı anda senkron yaptığı için paket yüklenemedi. Bir sonraki senkronda tekrar denenecek.");
				return result;
			}
			if (!put.transportOk || put.status < 200 || put.status >= 300)
			{
				result.error = HttpErrorText(put);
				return result;
			}

			// the cloud has this computer's version now
			{
				WriteBatch batch(db);
				for (const Mark &m : marks)
				{
					SyncRow &r = rows[m.guid];
					r.crc = m.crc;
					r.propsHash = m.props;
					r.contentHash = m.content;
					r.modified = m.modified;
					r.dirty = false;
					batch.Begin();
					SaveRow(db, r);
					batch.Done();
				}
				for (const auto &kv : newTombstones)
				{
					batch.Begin();
					DeleteRow(db, kv.first);
					batch.Done();
				}
				batch.Commit();
			}

			result.uploaded += uploadCount;
			result.deletedRemote += (int)newTombstones.size();
			result.remoteClips = (int)out.entries.size();
			result.ok = true;
			return result;
		}
	}
	catch (CppSQLite3Exception &e)
	{
		result.ok = false;
		result.error = _T("Veritabanı hatası: ") + CString(e.errorMessage());
	}
	return result;
}

// ===========================================================================
// CDbSync

BEGIN_MESSAGE_MAP(CDbSync, CWnd)
	ON_MESSAGE(WM_DB_SYNC_DONE, OnSyncDone)
END_MESSAGE_MAP()

CDbSync &CDbSync::Instance()
{
	static CDbSync instance;
	return instance;
}

CDbSync::CDbSync()
{
	::InitializeCriticalSection(&m_lock);
}

CDbSync::~CDbSync()
{
	::DeleteCriticalSection(&m_lock);
}

void CDbSync::Start()
{
	if (m_hWnd == NULL)
	{
		CString className = AfxRegisterWndClass(0);
		if (!CreateEx(WS_EX_TOOLWINDOW, className, _T("Ditto Sync"), WS_POPUP, CRect(0, 0, 0, 0), NULL, 0))
		{
			Log(_T("Sync: failed to create the controller window"));
			return;
		}
	}

	::EnterCriticalSection(&m_lock);
	m_settings.Load();
	::LeaveCriticalSection(&m_lock);

	if (m_thread == NULL)
	{
		InterlockedExchange(&m_stop, 0);
		m_stopEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL);
		m_syncEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
		m_reloadEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
		m_thread = ::CreateThread(NULL, 0, ThreadProc, this, 0, NULL);
		if (m_thread == NULL)
		{
			Log(_T("Sync: failed to start the worker thread"));
		}
	}
}

void CDbSync::Stop()
{
	InterlockedExchange(&m_stop, 1);
	if (m_thread != NULL)
	{
		::SetEvent(m_stopEvent);
		if (::WaitForSingleObject(m_thread, 5000) == WAIT_OBJECT_0)
		{
			::CloseHandle(m_stopEvent);
			::CloseHandle(m_syncEvent);
			::CloseHandle(m_reloadEvent);
		}
		else
		{
			// a request is still running; the process is exiting, leave its handles alone
			Log(_T("Sync: worker did not stop in time"));
		}
		::CloseHandle(m_thread);
		m_thread = NULL;
		m_stopEvent = m_syncEvent = m_reloadEvent = NULL;
	}
	if (m_hWnd != NULL)
	{
		DestroyWindow();
	}
}

void CDbSync::SettingsChanged()
{
	::EnterCriticalSection(&m_lock);
	m_settings.Load();
	::LeaveCriticalSection(&m_lock);
	if (m_reloadEvent != NULL)
	{
		::SetEvent(m_reloadEvent);
	}
}

void CDbSync::SyncNow()
{
	if (m_syncEvent != NULL)
	{
		::SetEvent(m_syncEvent);
	}
}

void CDbSync::StartTest(const DbSyncSettings &settings, HWND hNotify)
{
	TestParams *pParams = new TestParams{ settings, hNotify };
	HANDLE hThread = ::CreateThread(NULL, 0, TestThreadProc, pParams, 0, NULL);
	if (hThread != NULL)
	{
		::CloseHandle(hThread);
	}
	else
	{
		delete pParams;
		::PostMessage(hNotify, WM_DB_SYNC_TEST_DONE, 0, (LPARAM)new CString(_T("Test başlatılamadı.")));
	}
}

CString CDbSync::LastStatus()
{
	CString status = CGetSetOptions::GetProfileString(_T("DbSyncStatus"), _T(""));
	status.Replace(_T(" | "), _T("\r\n"));
	return status;
}

DbSyncSettings CDbSync::GetSettings()
{
	::EnterCriticalSection(&m_lock);
	DbSyncSettings copy = m_settings;
	::LeaveCriticalSection(&m_lock);
	return copy;
}

DWORD WINAPI CDbSync::ThreadProc(LPVOID param)
{
	((CDbSync *)param)->Run();
	return 0;
}

void CDbSync::Run()
{
	HANDLE handles[3] = { m_stopEvent, m_syncEvent, m_reloadEvent };
	bool first = true;

	while (true)
	{
		DbSyncSettings settings = GetSettings();
		DWORD timeout = INFINITE;
		if (settings.enabled && settings.autoSync)
		{
			timeout = (DWORD)settings.intervalMinutes * 60 * 1000;
		}
		if (first && settings.enabled && settings.syncOnStart)
		{
			timeout = 5000;		// let Ditto finish starting
		}
		first = false;

		DWORD wait = ::WaitForMultipleObjects(3, handles, FALSE, timeout);
		if (wait == WAIT_OBJECT_0 || m_stop)
		{
			break;
		}
		if (wait == WAIT_OBJECT_0 + 2)
		{
			continue;		// settings changed: schedule again
		}

		settings = GetSettings();
		bool manual = wait == WAIT_OBJECT_0 + 1;
		if (!settings.enabled && !manual)
		{
			continue;
		}

		InterlockedExchange(&m_running, 1);
		HWND hStatus = m_statusWnd;
		if (hStatus != NULL)
		{
			::PostMessage(hStatus, WM_DB_SYNC_STATUS, 0, 0);
		}

		DbSyncResult *pResult = new DbSyncResult(DbSyncRun(settings, CGetSetOptions::GetDBPath(), &m_stop, m_skipCache));

		InterlockedExchange(&m_running, 0);
		if (m_hWnd == NULL || !::PostMessage(m_hWnd, WM_DB_SYNC_DONE, 0, (LPARAM)pResult))
		{
			delete pResult;
		}
	}
}

LRESULT CDbSync::OnSyncDone(WPARAM, LPARAM lParam)
{
	std::unique_ptr<DbSyncResult> result((DbSyncResult *)lParam);

	// one line: the portable version keeps settings in an ini file
	CString summary = result->Summary();
	summary.Replace(_T("\r\n"), _T(" | "));
	CGetSetOptions::SetProfileString(_T("DbSyncStatus"), summary);
	Log(_T("Sync: ") + summary);

	if (result->localChanged)
	{
		theApp.RefreshView();
	}
	if (m_statusWnd != NULL && ::IsWindow(m_statusWnd))
	{
		::PostMessage(m_statusWnd, WM_DB_SYNC_STATUS, 0, 0);
	}
	return 0;
}
