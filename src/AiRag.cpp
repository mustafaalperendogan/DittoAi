#include "stdafx.h"
#include "AiRag.h"
#include "AiJson.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace
{
	const ULONGLONG MAX_FILE_BYTES = 4 * 1024 * 1024;
	const int MAX_FILES = 5000;
	const int MAX_CHUNKS = 50000;
	const int MAX_DEPTH = 12;
	const double BM25_K1 = 1.2;
	const double BM25_B = 0.75;

	LPCTSTR SUPPORTED_EXTENSIONS[] =
	{
		_T("txt"), _T("md"), _T("markdown"), _T("csv"), _T("tsv"), _T("json"), _T("xml"), _T("html"), _T("htm"),
		_T("log"), _T("ini"), _T("yaml"), _T("yml"), _T("rst"), _T("tex"), _T("sql"),
		_T("c"), _T("cpp"), _T("h"), _T("hpp"), _T("cs"), _T("java"), _T("py"), _T("js"), _T("ts"), _T("go"),
		_T("rb"), _T("php"), _T("sh"), _T("ps1"), _T("bat"), _T("css"), _T("vb"), _T("kt"), _T("swift"), _T("rs")
	};

	struct Chunk
	{
		CString source;
		CString text;
		std::unordered_map<std::wstring, int> termCounts;
		int length = 0;
	};

	struct Index
	{
		CString signature;
		std::vector<Chunk> chunks;
		std::unordered_map<std::wstring, int> documentFrequency;
		double averageLength = 0;
		AiRagStats stats;
	};

	struct FileEntry
	{
		CString path;
		ULONGLONG size;
		FILETIME modified;
	};

	struct CriticalSection
	{
		CRITICAL_SECTION cs;
		CriticalSection() { ::InitializeCriticalSection(&cs); }
		~CriticalSection() { ::DeleteCriticalSection(&cs); }
	};

	CRITICAL_SECTION &Lock()
	{
		static CriticalSection lock;	// thread safe initialization (C++11 magic statics)
		return lock.cs;
	}

	std::map<CString, std::shared_ptr<Index>> &Cache()
	{
		static std::map<CString, std::shared_ptr<Index>> cache;
		return cache;
	}

	// ---------------------------------------------------------------- files

	void CollectFiles(const CString &path, std::vector<FileEntry> &files, int depth, AiRagStats &stats)
	{
		if ((int)files.size() >= MAX_FILES || depth > MAX_DEPTH)
		{
			return;
		}

		DWORD attributes = ::GetFileAttributes(path);
		if (attributes == INVALID_FILE_ATTRIBUTES)
		{
			return;
		}

		if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			WIN32_FILE_ATTRIBUTE_DATA data;
			if (!AiRagIsSupportedFile(path) || !::GetFileAttributesEx(path, GetFileExInfoStandard, &data))
			{
				stats.skippedFiles++;
				return;
			}
			ULONGLONG size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
			if (size > MAX_FILE_BYTES)
			{
				stats.skippedFiles++;
				return;
			}
			files.push_back({ path, size, data.ftLastWriteTime });
			return;
		}

		CString folder = path;
		folder.TrimRight(_T('\\'));
		WIN32_FIND_DATA fd;
		HANDLE hFind = ::FindFirstFile(folder + _T("\\*"), &fd);
		if (hFind == INVALID_HANDLE_VALUE)
		{
			return;
		}

		std::vector<CString> names;
		do
		{
			CString name = fd.cFileName;
			if (name == _T(".") || name == _T(".."))
			{
				continue;
			}
			// skip hidden and version control / build folders
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) || name == _T(".git") || name == _T("node_modules") || name == _T("bin") || name == _T("obj"))
			{
				continue;
			}
			names.push_back(name);
		} while (::FindNextFile(hFind, &fd));
		::FindClose(hFind);

		std::sort(names.begin(), names.end(), [](const CString &a, const CString &b) { return a.CompareNoCase(b) < 0; });
		for (const CString &name : names)
		{
			CollectFiles(folder + _T("\\") + name, files, depth + 1, stats);
		}
	}

	CString ReadTextFile(const CString &path)
	{
		CFile file;
		if (!file.Open(path, CFile::modeRead | CFile::shareDenyNone))
		{
			return CString();
		}
		ULONGLONG length = file.GetLength();
		std::string bytes((size_t)length, '\0');
		if (length > 0)
		{
			file.Read(&bytes[0], (UINT)length);
		}
		file.Close();

		if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE)
		{
			return CString((LPCWSTR)(bytes.data() + 2), (int)((bytes.size() - 2) / 2));
		}
		if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFE && (unsigned char)bytes[1] == 0xFF)
		{
			std::wstring w((bytes.size() - 2) / 2, L'\0');
			for (size_t i = 0; i < w.size(); i++)
			{
				w[i] = (wchar_t)(((unsigned char)bytes[2 + i * 2] << 8) | (unsigned char)bytes[3 + i * 2]);
			}
			return CString(w.c_str(), (int)w.size());
		}

		size_t start = 0;
		if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
		{
			start = 3;
		}

		const char *data = bytes.data() + start;
		int count = (int)(bytes.size() - start);
		if (count == 0)
		{
			return CString();
		}

		UINT codePage = CP_UTF8;
		int len = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, count, NULL, 0);
		if (len == 0)
		{
			codePage = CP_ACP;	// not UTF-8, assume the system code page (e.g. Windows-1254)
			len = ::MultiByteToWideChar(CP_ACP, 0, data, count, NULL, 0);
		}
		CString text;
		::MultiByteToWideChar(codePage, 0, data, count, text.GetBuffer(len), len);
		text.ReleaseBuffer(len);
		return text;
	}

	CString StripMarkup(const CString &text)
	{
		CString out;
		out.Preallocate(text.GetLength());
		bool inTag = false;
		for (int i = 0; i < text.GetLength(); i++)
		{
			TCHAR c = text[i];
			if (c == _T('<')) { inTag = true; out += _T(' '); continue; }
			if (c == _T('>')) { inTag = false; continue; }
			if (!inTag) out += c;
		}
		out.Replace(_T("&nbsp;"), _T(" "));
		out.Replace(_T("&amp;"), _T("&"));
		out.Replace(_T("&lt;"), _T("<"));
		out.Replace(_T("&gt;"), _T(">"));
		out.Replace(_T("&quot;"), _T("\""));
		return out;
	}

	// ---------------------------------------------------------------- text processing

	// Lower case that also folds Turkish dotted/dotless i, so "İstanbul", "ISTANBUL" and "istanbul" match.
	wchar_t FoldChar(wchar_t c)
	{
		if (c == L'I' || c == 0x0130 || c == 0x0131) return L'i';
		wchar_t buf[2] = { c, 0 };
		::CharLowerBuffW(buf, 1);
		return buf[0];
	}

	void Tokenize(const CString &text, std::vector<std::wstring> &tokens)
	{
		std::wstring current;
		int len = text.GetLength();
		for (int i = 0; i <= len; i++)
		{
			wchar_t c = i < len ? text[i] : L' ';
			if (c != L' ' && ::IsCharAlphaNumericW(c))
			{
				current += FoldChar(c);
			}
			else
			{
				if (current.size() >= 2 && current.size() <= 40)
				{
					tokens.push_back(current);
				}
				current.clear();
			}
		}
	}

	void SplitIntoChunks(const CString &text, int chunkChars, std::vector<CString> &out)
	{
		CString normalized = text;
		normalized.Replace(_T("\r\n"), _T("\n"));

		CString current;
		auto flush = [&]()
		{
			current.Trim();
			if (current.IsEmpty() == FALSE)
			{
				out.push_back(current);
			}
			current.Empty();
		};

		int pos = 0;
		int len = normalized.GetLength();
		while (pos < len)
		{
			// next paragraph
			int end = normalized.Find(_T("\n\n"), pos);
			if (end < 0) end = len;
			CString paragraph = normalized.Mid(pos, end - pos);
			pos = end + 2;

			paragraph.Trim();
			if (paragraph.IsEmpty())
			{
				continue;
			}

			if (current.GetLength() + paragraph.GetLength() + 2 <= chunkChars)
			{
				if (current.IsEmpty() == FALSE) current += _T("\n\n");
				current += paragraph;
				continue;
			}

			flush();

			// a long paragraph is cut at spaces with a small overlap
			while (paragraph.GetLength() > chunkChars)
			{
				int cut = paragraph.Left(chunkChars).ReverseFind(_T(' '));
				if (cut < chunkChars / 2) cut = chunkChars;
				out.push_back(paragraph.Left(cut).Trim());
				int overlapStart = max(0, cut - 120);
				int space = paragraph.Find(_T(' '), overlapStart);
				int next = (space >= 0 && space < cut) ? space + 1 : cut;
				paragraph = paragraph.Mid(next);
			}
			current = paragraph;
		}
		flush();
	}

	CString BuildSignature(const AiModelConfig &model, const std::vector<FileEntry> &files)
	{
		CString signature;
		signature.Format(_T("chunk=%d;"), model.ragChunkChars);
		for (const FileEntry &f : files)
		{
			signature.AppendFormat(_T("%s|%I64u|%08x%08x;"), (LPCTSTR)f.path, f.size, f.modified.dwHighDateTime, f.modified.dwLowDateTime);
		}
		return signature;
	}

	int ChunkChars(const AiModelConfig &model)
	{
		return min(max(model.ragChunkChars, 300), 8000);
	}

	std::shared_ptr<Index> BuildIndex(const AiModelConfig &model, const std::vector<FileEntry> &files, const CString &signature, AiRagStats stats)
	{
		auto index = std::make_shared<Index>();
		index->signature = signature;

		int chunkChars = ChunkChars(model);
		long long totalLength = 0;

		for (const FileEntry &f : files)
		{
			CString text = ReadTextFile(f.path);
			CString ext = ::PathFindExtension(f.path);
			ext.MakeLower();
			if (ext == _T(".html") || ext == _T(".htm") || ext == _T(".xml"))
			{
				text = StripMarkup(text);
			}
			if (text.Trim().IsEmpty())
			{
				stats.skippedFiles++;
				continue;
			}
			stats.files++;

			std::vector<CString> pieces;
			SplitIntoChunks(text, chunkChars, pieces);
			for (const CString &piece : pieces)
			{
				if ((int)index->chunks.size() >= MAX_CHUNKS)
				{
					break;
				}

				Chunk chunk;
				chunk.source = f.path;
				chunk.text = piece;

				std::vector<std::wstring> tokens;
				Tokenize(piece, tokens);
				if (tokens.empty())
				{
					continue;
				}
				for (const std::wstring &t : tokens)
				{
					chunk.termCounts[t]++;
				}
				chunk.length = (int)tokens.size();
				totalLength += chunk.length;

				for (auto &tc : chunk.termCounts)
				{
					index->documentFrequency[tc.first]++;
				}
				index->chunks.push_back(std::move(chunk));
			}
		}

		stats.chunks = (int)index->chunks.size();
		index->averageLength = index->chunks.empty() ? 0 : (double)totalLength / index->chunks.size();
		index->stats = stats;
		return index;
	}

	std::shared_ptr<Index> GetIndex(const AiModelConfig &model, bool forceRebuild, AiRagStats &stats)
	{
		std::vector<FileEntry> files;
		AiRagStats scan;
		for (const CString &source : model.ragSources)
		{
			CString path = source;
			path.Trim();
			if (path.IsEmpty() == FALSE)
			{
				CollectFiles(path, files, 0, scan);
			}
		}

		CString signature = BuildSignature(model, files);

		::EnterCriticalSection(&Lock());
		auto it = Cache().find(model.id);
		std::shared_ptr<Index> cached = it != Cache().end() ? it->second : nullptr;
		::LeaveCriticalSection(&Lock());

		if (cached && !forceRebuild && cached->signature == signature)
		{
			stats = cached->stats;
			return cached;
		}

		std::shared_ptr<Index> index = BuildIndex(model, files, signature, scan);

		::EnterCriticalSection(&Lock());
		Cache()[model.id] = index;
		::LeaveCriticalSection(&Lock());

		stats = index->stats;
		return index;
	}

	CString EscapeAttribute(CString value)
	{
		value.Replace(_T("&"), _T("&amp;"));
		value.Replace(_T("\""), _T("&quot;"));
		value.Replace(_T("<"), _T("&lt;"));
		return value;
	}
}

bool AiRagIsSupportedFile(const CString &path)
{
	CString ext = ::PathFindExtension(path);
	ext.TrimLeft(_T('.'));
	for (LPCTSTR supported : SUPPORTED_EXTENSIONS)
	{
		if (ext.CompareNoCase(supported) == 0)
		{
			return true;
		}
	}
	return false;
}

AiRagStats AiRagBuildIndex(const AiModelConfig &model)
{
	AiRagStats stats;
	if (model.ragSources.empty())
	{
		stats.error = _T("Hiç kaynak eklenmemiş.");
		return stats;
	}
	GetIndex(model, true, stats);
	if (stats.files == 0)
	{
		stats.error = _T("Desteklenen metin dosyası bulunamadı.");
	}
	return stats;
}

CString AiRagRetrieve(const AiModelConfig &model, const CString &query, AiRagStats *statsOut)
{
	AiRagStats stats;
	if (!model.ragEnabled || model.ragSources.empty())
	{
		if (statsOut) *statsOut = stats;
		return CString();
	}

	std::shared_ptr<Index> index = GetIndex(model, false, stats);
	if (statsOut) *statsOut = stats;
	if (!index || index->chunks.empty())
	{
		return CString();
	}

	std::vector<std::wstring> queryTokens;
	Tokenize(query, queryTokens);
	std::sort(queryTokens.begin(), queryTokens.end());
	queryTokens.erase(std::unique(queryTokens.begin(), queryTokens.end()), queryTokens.end());
	if (queryTokens.empty())
	{
		return CString();
	}

	double n = (double)index->chunks.size();
	std::vector<std::pair<double, size_t>> scores;
	for (size_t i = 0; i < index->chunks.size(); i++)
	{
		const Chunk &chunk = index->chunks[i];
		double score = 0;
		for (const std::wstring &term : queryTokens)
		{
			auto tf = chunk.termCounts.find(term);
			if (tf == chunk.termCounts.end())
			{
				continue;
			}
			double df = index->documentFrequency[term];
			double idf = std::log(1.0 + (n - df + 0.5) / (df + 0.5));
			double freq = tf->second;
			score += idf * (freq * (BM25_K1 + 1)) / (freq + BM25_K1 * (1 - BM25_B + BM25_B * chunk.length / max(index->averageLength, 1.0)));
		}
		if (score > 0)
		{
			scores.push_back({ score, i });
		}
	}

	if (scores.empty())
	{
		return CString();
	}

	int topK = min(max(model.ragTopK, 1), 20);
	size_t take = min((size_t)topK, scores.size());
	std::partial_sort(scores.begin(), scores.begin() + take, scores.end(),
		[](const std::pair<double, size_t> &a, const std::pair<double, size_t> &b) { return a.first > b.first; });

	CString context = _T("The following excerpts come from the user's own reference documents. Use them when they are relevant ")
		_T("to the task (facts, names, terminology, style); ignore them otherwise. Do not mention that you were given documents.\n<documents>\n");
	for (size_t i = 0; i < take; i++)
	{
		const Chunk &chunk = index->chunks[scores[i].second];
		context.AppendFormat(_T("<document index=\"%d\" source=\"%s\">\n%s\n</document>\n"),
			(int)i + 1, (LPCTSTR)EscapeAttribute(::PathFindFileName(chunk.source)), (LPCTSTR)chunk.text);
	}
	context += _T("</documents>");
	return context;
}
