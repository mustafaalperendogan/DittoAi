#include "stdafx.h"
#include "AiConfig.h"
#include "AiJson.h"
#include "Options.h"
#include "Misc.h"

#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

namespace
{
	const int CONFIG_VERSION = 1;
	const LPCTSTR DPAPI_PREFIX = _T("dpapi:");

	CString ProtectString(const CString &plain)
	{
		if (plain.IsEmpty())
		{
			return CString();
		}

		std::string utf8 = AiToUtf8(plain);
		DATA_BLOB in = { (DWORD)utf8.size(), (BYTE *)utf8.data() };
		DATA_BLOB out = { 0 };
		if (!::CryptProtectData(&in, L"Ditto AI", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out))
		{
			return CString();
		}

		DWORD chars = 0;
		::CryptBinaryToString(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &chars);
		CString base64;
		::CryptBinaryToString(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, base64.GetBuffer(chars), &chars);
		base64.ReleaseBuffer();
		::LocalFree(out.pbData);

		return DPAPI_PREFIX + base64;
	}

	CString UnprotectString(const CString &stored)
	{
		int prefixLen = (int)_tcslen(DPAPI_PREFIX);
		if (stored.Left(prefixLen) != DPAPI_PREFIX)
		{
			return CString();
		}

		CString base64 = stored.Mid(prefixLen);
		DWORD size = 0;
		if (!::CryptStringToBinary(base64, base64.GetLength(), CRYPT_STRING_BASE64, NULL, &size, NULL, NULL))
		{
			return CString();
		}
		std::vector<BYTE> bytes(size);
		if (!::CryptStringToBinary(base64, base64.GetLength(), CRYPT_STRING_BASE64, bytes.data(), &size, NULL, NULL))
		{
			return CString();
		}

		DATA_BLOB in = { size, bytes.data() };
		DATA_BLOB out = { 0 };
		if (!::CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out))
		{
			return CString();
		}
		CString plain = AiFromUtf8(std::string((char *)out.pbData, out.cbData));
		::SecureZeroMemory(out.pbData, out.cbData);
		::LocalFree(out.pbData);
		return plain;
	}

	CString GetEnv(LPCTSTR name)
	{
		CString value;
		DWORD len = ::GetEnvironmentVariable(name, NULL, 0);
		if (len > 0)
		{
			::GetEnvironmentVariable(name, value.GetBuffer(len), len);
			value.ReleaseBuffer();
		}
		return value.Trim();
	}

	AiAction MakeAction(LPCTSTR name, LPCTSTR instruction, AiContentType type)
	{
		AiAction a;
		a.id = AiConfig::NewId();
		a.name = name;
		a.instruction = instruction;
		a.appliesTo = type;
		return a;
	}
}

// ===========================================================================
// AiModelConfig

CString AiModelConfig::ProviderName(AiProvider provider)
{
	switch (provider)
	{
	case AiProvider::Anthropic: return _T("Anthropic (Claude)");
	case AiProvider::OpenAI: return _T("OpenAI");
	case AiProvider::OpenAICompatible: return _T("OpenAI uyumlu (Ollama, LM Studio, OpenRouter...)");
	case AiProvider::Gemini: return _T("Google Gemini");
	default: return _T("?");
	}
}

CString AiModelConfig::DefaultBaseUrl(AiProvider provider)
{
	switch (provider)
	{
	case AiProvider::Anthropic: return _T("https://api.anthropic.com/v1");
	case AiProvider::OpenAI: return _T("https://api.openai.com/v1");
	case AiProvider::OpenAICompatible: return _T("http://localhost:11434/v1");
	case AiProvider::Gemini: return _T("https://generativelanguage.googleapis.com/v1beta");
	default: return CString();
	}
}

CString AiModelConfig::DefaultModel(AiProvider provider)
{
	switch (provider)
	{
	case AiProvider::Anthropic: return _T("claude-opus-5-5");
	default: return CString();		// the user knows which model their account/server offers
	}
}

CString AiModelConfig::EffectiveBaseUrl() const
{
	CString url = baseUrl;
	url.Trim();
	if (url.IsEmpty())
	{
		url = DefaultBaseUrl(provider);
	}
	url.TrimRight(_T('/'));
	return url;
}

CString AiModelConfig::EnvironmentKeyName() const
{
	switch (provider)
	{
	case AiProvider::Anthropic: return _T("ANTHROPIC_API_KEY");
	case AiProvider::OpenAI: return _T("OPENAI_API_KEY");
	case AiProvider::Gemini: return _T("GEMINI_API_KEY");
	default: return CString();
	}
}

CString AiModelConfig::ResolveApiKey() const
{
	CString key = apiKey;
	key.Trim();
	if (key.IsEmpty() == FALSE)
	{
		return key;
	}

	CString envName = EnvironmentKeyName();
	if (envName.IsEmpty() == FALSE)
	{
		key = GetEnv(envName);
	}
	if (key.IsEmpty() && provider == AiProvider::Gemini)
	{
		key = GetEnv(_T("GOOGLE_API_KEY"));
	}
	return key;
}

CString AiModelConfig::ExamplesToText(const std::vector<AiExample> &examples)
{
	CString text;
	for (const AiExample &e : examples)
	{
		if (text.IsEmpty() == FALSE)
		{
			text += _T("\r\n\r\n");
		}
		CString input = e.input, output = e.output;
		input.Replace(_T("\r\n"), _T("\n")); input.Replace(_T("\n"), _T("\r\n"));
		output.Replace(_T("\r\n"), _T("\n")); output.Replace(_T("\n"), _T("\r\n"));
		text += _T(">>>\r\n") + input + _T("\r\n<<<\r\n") + output;
	}
	return text;
}

std::vector<AiExample> AiModelConfig::ExamplesFromText(const CString &text)
{
	std::vector<AiExample> list;

	CString normalized = text;
	normalized.Replace(_T("\r\n"), _T("\n"));

	AiExample current;
	int mode = 0;	// 0 none, 1 input, 2 output

	auto flush = [&]()
	{
		current.input.Trim();
		current.output.Trim();
		if (current.input.IsEmpty() == FALSE && current.output.IsEmpty() == FALSE)
		{
			list.push_back(current);
		}
		current = AiExample();
	};

	int start = 0;
	while (start <= normalized.GetLength())
	{
		int end = normalized.Find(_T('\n'), start);
		if (end < 0) end = normalized.GetLength();
		CString line = normalized.Mid(start, end - start);
		start = end + 1;

		CString trimmed = line;
		trimmed.Trim();
		if (trimmed.Left(3) == _T(">>>"))
		{
			if (mode != 0) flush();
			mode = 1;
			continue;
		}
		if (trimmed.Left(3) == _T("<<<"))
		{
			mode = 2;
			continue;
		}

		if (mode == 1) current.input += line + _T("\n");
		else if (mode == 2) current.output += line + _T("\n");

		if (end >= normalized.GetLength()) break;
	}
	if (mode != 0) flush();

	return list;
}

// ===========================================================================
// AiConfig

CString AiConfig::NewId()
{
	GUID guid;
	::CoCreateGuid(&guid);
	CString id;
	id.Format(_T("%08lx%04x%04x"), guid.Data1, guid.Data2, guid.Data3);
	return id;
}

CString AiConfig::FilePath()
{
	CString path = CGetSetOptions::GetAppDataPath();
	FIX_CSTRING_PATH(path);
	return path + _T("AiConfig.json");
}

void AiConfig::SetDefaults()
{
	*this = AiConfig();

	AiModelConfig claude;
	claude.id = NewId();
	claude.name = _T("Claude Opus 5.5");
	claude.provider = AiProvider::Anthropic;
	claude.model = AiModelConfig::DefaultModel(AiProvider::Anthropic);
	claude.supportsImages = true;
	claude.effort = _T("low");
	models.push_back(claude);

	defaultTextModelId = claude.id;
	defaultImageModelId = claude.id;

	actions.push_back(MakeAction(_T("Kurumsal e-postaya çevir"), _T("Bu metni profesyonel, kibar ve net bir kurumsal e-postaya dönüştür. Uygun bir hitap ve kapanış ekle."), AiContentType::Text));
	actions.push_back(MakeAction(_T("Yazım ve dilbilgisini düzelt"), _T("Yazım, noktalama ve dilbilgisi hatalarını düzelt. Anlamı ve üslubu değiştirme."), AiContentType::Text));
	actions.push_back(MakeAction(_T("Daha kibar ve profesyonel yap"), _T("Metni anlamını koruyarak daha kibar ve profesyonel bir üsluba çevir."), AiContentType::Text));
	actions.push_back(MakeAction(_T("Kısalt"), _T("Metni ana fikrini koruyarak belirgin şekilde kısalt."), AiContentType::Text));
	actions.push_back(MakeAction(_T("Madde madde özetle"), _T("Metni kısa maddeler halinde özetle."), AiContentType::Text));
	actions.push_back(MakeAction(_T("İngilizceye çevir"), _T("Metni doğal ve akıcı bir İngilizceye çevir."), AiContentType::Text));
	actions.push_back(MakeAction(_T("Türkçeye çevir"), _T("Metni doğal ve akıcı bir Türkçeye çevir."), AiContentType::Text));
	actions.push_back(MakeAction(_T("Görseli açıkla"), _T("Bu görselde ne olduğunu kısa ve net şekilde Türkçe açıkla."), AiContentType::Image));
	actions.push_back(MakeAction(_T("Görseldeki metni çıkar"), _T("Görseldeki tüm metni, satır düzenini koruyarak olduğu gibi yaz. Başka bir şey ekleme."), AiContentType::Image));
	actions.push_back(MakeAction(_T("Görseldeki tabloyu metne çevir"), _T("Görseldeki tabloyu sekmeyle ayrılmış sütunlar halinde yaz, Excel'e yapıştırılabilsin."), AiContentType::Image));
}

const AiModelConfig *AiConfig::FindModel(const CString &id) const
{
	if (id.IsEmpty())
	{
		return nullptr;
	}
	for (const AiModelConfig &m : models)
	{
		if (m.id == id)
		{
			return &m;
		}
	}
	return nullptr;
}

const AiModelConfig *AiConfig::DefaultModelFor(bool image) const
{
	const AiModelConfig *model = FindModel(image ? defaultImageModelId : defaultTextModelId);
	if (model != nullptr)
	{
		return model;
	}

	for (const AiModelConfig &m : models)
	{
		if (!image || m.supportsImages)
		{
			return &m;
		}
	}
	return nullptr;
}

const AiModelConfig *AiConfig::ResolveModel(const AiAction &action, bool image) const
{
	const AiModelConfig *model = FindModel(action.modelId);
	return model != nullptr ? model : DefaultModelFor(image);
}

void AiConfig::OnModelRemoved(const CString &id)
{
	for (AiAction &a : actions)
	{
		if (a.modelId == id)
		{
			a.modelId.Empty();
		}
	}
	if (defaultTextModelId == id) defaultTextModelId.Empty();
	if (defaultImageModelId == id) defaultImageModelId.Empty();
}

bool AiConfig::Load()
{
	SetDefaults();

	CFile file;
	if (!file.Open(FilePath(), CFile::modeRead | CFile::shareDenyWrite))
	{
		return false;
	}

	ULONGLONG length = file.GetLength();
	if (length == 0 || length > 50 * 1024 * 1024)
	{
		return false;
	}
	std::string text((size_t)length, '\0');
	file.Read(&text[0], (UINT)length);
	file.Close();

	AiJson root;
	if (!AiJson::Parse(text, root) || root.type != AiJson::Object)
	{
		Log(_T("AI: AiConfig.json could not be parsed, using defaults"));
		return false;
	}

	AiConfig loaded;
	loaded.enabled = root.GetBool("enabled", true);
	loaded.doubleCopyMs = (int)root.GetNumber("doubleCopyMs", 400);
	if (loaded.doubleCopyMs < 150 || loaded.doubleCopyMs > 2000) loaded.doubleCopyMs = 400;
	loaded.defaultTextModelId = root.GetCString("defaultTextModel");
	loaded.defaultImageModelId = root.GetCString("defaultImageModel");

	for (const AiJson &jm : root.GetArray("models"))
	{
		AiModelConfig m;
		m.id = jm.GetCString("id", NewId());
		m.name = jm.GetCString("name");
		int provider = (int)jm.GetNumber("provider", 0);
		m.provider = (provider >= 0 && provider < (int)AiProvider::Count) ? (AiProvider)provider : AiProvider::Anthropic;
		m.baseUrl = jm.GetCString("baseUrl");
		m.apiKey = UnprotectString(jm.GetCString("apiKey"));
		m.authHeader = jm.GetCString("authHeader");
		m.model = jm.GetCString("model");
		m.supportsImages = jm.GetBool("supportsImages", true);
		m.systemPrompt = jm.GetCString("systemPrompt");
		m.temperature = jm.GetNumber("temperature", -1);
		m.maxTokens = (int)jm.GetNumber("maxTokens", 8192);
		m.effort = jm.GetCString("effort");
		for (const AiJson &je : jm.GetArray("examples"))
		{
			AiExample e;
			e.input = je.GetCString("input");
			e.output = je.GetCString("output");
			m.examples.push_back(e);
		}
		m.ragEnabled = jm.GetBool("ragEnabled", false);
		for (const AiJson &js : jm.GetArray("ragSources"))
		{
			if (js.type == AiJson::String)
			{
				m.ragSources.push_back(AiFromUtf8(js.str));
			}
		}
		m.ragTopK = (int)jm.GetNumber("ragTopK", 4);
		m.ragChunkChars = (int)jm.GetNumber("ragChunkChars", 1200);
		if (m.name.IsEmpty()) m.name = m.model;
		loaded.models.push_back(m);
	}

	for (const AiJson &ja : root.GetArray("actions"))
	{
		AiAction a;
		a.id = ja.GetCString("id", NewId());
		a.name = ja.GetCString("name");
		a.instruction = ja.GetCString("instruction");
		int type = (int)ja.GetNumber("appliesTo", 0);
		a.appliesTo = (type >= 0 && type <= 2) ? (AiContentType)type : AiContentType::Text;
		a.modelId = ja.GetCString("model");
		if (a.name.IsEmpty() == FALSE)
		{
			loaded.actions.push_back(a);
		}
	}

	if (const AiJson *jn = root.Get("notifications"))
	{
		loaded.notify.showProgress = jn->GetBool("showProgress", true);
		loaded.notify.showSuccess = jn->GetBool("showSuccess", true);
		loaded.notify.showErrors = jn->GetBool("showErrors", true);
		loaded.notify.position = (int)jn->GetNumber("position", 0);
		if (loaded.notify.position < 0 || loaded.notify.position > 3) loaded.notify.position = 0;
		loaded.notify.durationSec = (int)jn->GetNumber("durationSec", 6);
		if (loaded.notify.durationSec < 1 || loaded.notify.durationSec > 120) loaded.notify.durationSec = 6;
		loaded.notify.sound = jn->GetBool("sound", false);
		loaded.notify.showPreview = jn->GetBool("showPreview", true);
	}

	*this = loaded;
	return true;
}

bool AiConfig::Save(CString *error) const
{
	AiJsonWriter w;
	w.BeginObject();
	w.Int("version", CONFIG_VERSION);
	w.Bool("enabled", enabled);
	w.Int("doubleCopyMs", doubleCopyMs);
	w.Str("defaultTextModel", defaultTextModelId);
	w.Str("defaultImageModel", defaultImageModelId);

	w.BeginArray("models");
	for (const AiModelConfig &m : models)
	{
		w.BeginObject();
		w.Str("id", m.id);
		w.Str("name", m.name);
		w.Int("provider", (int)m.provider);
		w.Str("baseUrl", m.baseUrl);
		w.Str("apiKey", ProtectString(m.apiKey));
		w.Str("authHeader", m.authHeader);
		w.Str("model", m.model);
		w.Bool("supportsImages", m.supportsImages);
		w.Str("systemPrompt", m.systemPrompt);
		w.Num("temperature", m.temperature);
		w.Int("maxTokens", m.maxTokens);
		w.Str("effort", m.effort);
		w.BeginArray("examples");
		for (const AiExample &e : m.examples)
		{
			w.BeginObject();
			w.Str("input", e.input);
			w.Str("output", e.output);
			w.EndObject();
		}
		w.EndArray();
		w.Bool("ragEnabled", m.ragEnabled);
		w.BeginArray("ragSources");
		for (const CString &s : m.ragSources)
		{
			w.Str(nullptr, s);
		}
		w.EndArray();
		w.Int("ragTopK", m.ragTopK);
		w.Int("ragChunkChars", m.ragChunkChars);
		w.EndObject();
	}
	w.EndArray();

	w.BeginArray("actions");
	for (const AiAction &a : actions)
	{
		w.BeginObject();
		w.Str("id", a.id);
		w.Str("name", a.name);
		w.Str("instruction", a.instruction);
		w.Int("appliesTo", (int)a.appliesTo);
		w.Str("model", a.modelId);
		w.EndObject();
	}
	w.EndArray();

	w.BeginObject("notifications");
	w.Bool("showProgress", notify.showProgress);
	w.Bool("showSuccess", notify.showSuccess);
	w.Bool("showErrors", notify.showErrors);
	w.Int("position", notify.position);
	w.Int("durationSec", notify.durationSec);
	w.Bool("sound", notify.sound);
	w.Bool("showPreview", notify.showPreview);
	w.EndObject();

	w.EndObject();

	// Write to a temp file first so a crash never leaves a half written config.
	CString path = FilePath();
	CString folder = path.Left(path.ReverseFind(_T('\\')));
	::SHCreateDirectoryEx(NULL, folder, NULL);

	CString tempPath = path + _T(".tmp");
	CFile file;
	CFileException ex;
	if (!file.Open(tempPath, CFile::modeCreate | CFile::modeWrite, &ex))
	{
		if (error) error->Format(_T("Ayar dosyası yazılamadı: %s"), (LPCTSTR)tempPath);
		return false;
	}
	const std::string &json = w.Result();
	file.Write(json.data(), (UINT)json.size());
	file.Close();

	if (!::MoveFileEx(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		if (error) error->Format(_T("Ayar dosyası kaydedilemedi: %s"), (LPCTSTR)path);
		return false;
	}
	return true;
}
