#include "stdafx.h"
#include "AiUsage.h"
#include "AiJson.h"
#include "Options.h"
#include "Misc.h"

namespace
{
	const int USAGE_VERSION = 1;

	bool SaveEntries(const std::vector<AiUsageEntry> &entries, CString *error)
	{
		AiJsonWriter w;
		w.BeginObject();
		w.Int("version", USAGE_VERSION);
		w.BeginArray("days");
		for (const AiUsageEntry &e : entries)
		{
			w.BeginObject();
			w.Str("date", e.date);
			w.Str("modelId", e.modelId);
			w.Str("modelName", e.modelName);
			w.Str("model", e.model);
			w.Int("requests", e.requests);
			w.Int("unknownRequests", e.unknownRequests);
			w.Int("inputTokens", e.inputTokens);
			w.Int("outputTokens", e.outputTokens);
			w.EndObject();
		}
		w.EndArray();
		w.EndObject();

		// Write to a temp file first so a crash never leaves a half written file.
		CString path = AiUsageFilePath();
		CString folder = path.Left(path.ReverseFind(_T('\\')));
		::SHCreateDirectoryEx(NULL, folder, NULL);

		CString tempPath = path + _T(".tmp");
		CFile file;
		if (!file.Open(tempPath, CFile::modeCreate | CFile::modeWrite))
		{
			if (error) error->Format(_T("Kullanım dosyası yazılamadı: %s"), (LPCTSTR)tempPath);
			return false;
		}
		const std::string &json = w.Result();
		file.Write(json.data(), (UINT)json.size());
		file.Close();

		if (!::MoveFileEx(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			if (error) error->Format(_T("Kullanım dosyası kaydedilemedi: %s"), (LPCTSTR)path);
			return false;
		}
		return true;
	}
}

CString AiUsageFilePath()
{
	CString path = CGetSetOptions::GetAppDataPath();
	FIX_CSTRING_PATH(path);
	return path + _T("AiUsage.json");
}

CString AiUsageDate(int daysAgo)
{
	SYSTEMTIME st;
	::GetLocalTime(&st);
	if (daysAgo != 0)
	{
		FILETIME ft;
		::SystemTimeToFileTime(&st, &ft);
		ULARGE_INTEGER value;
		value.LowPart = ft.dwLowDateTime;
		value.HighPart = ft.dwHighDateTime;
		value.QuadPart -= (ULONGLONG)daysAgo * 24 * 60 * 60 * 10000000ULL;
		ft.dwLowDateTime = value.LowPart;
		ft.dwHighDateTime = value.HighPart;
		::FileTimeToSystemTime(&ft, &st);
	}
	CString date;
	date.Format(_T("%04d-%02d-%02d"), st.wYear, st.wMonth, st.wDay);
	return date;
}

std::vector<AiUsageEntry> AiUsageLoad()
{
	std::vector<AiUsageEntry> entries;

	CFile file;
	if (!file.Open(AiUsageFilePath(), CFile::modeRead | CFile::shareDenyWrite))
	{
		return entries;
	}
	ULONGLONG length = file.GetLength();
	if (length == 0 || length > 50 * 1024 * 1024)
	{
		return entries;
	}
	std::string text((size_t)length, '\0');
	file.Read(&text[0], (UINT)length);
	file.Close();

	AiJson root;
	if (!AiJson::Parse(text, root) || root.type != AiJson::Object)
	{
		Log(_T("AI: AiUsage.json could not be parsed"));
		return entries;
	}

	for (const AiJson &jd : root.GetArray("days"))
	{
		AiUsageEntry e;
		e.date = jd.GetCString("date");
		e.modelId = jd.GetCString("modelId");
		e.modelName = jd.GetCString("modelName");
		e.model = jd.GetCString("model");
		e.requests = (int)jd.GetNumber("requests", 0);
		e.unknownRequests = (int)jd.GetNumber("unknownRequests", 0);
		e.inputTokens = (long long)jd.GetNumber("inputTokens", 0);
		e.outputTokens = (long long)jd.GetNumber("outputTokens", 0);
		if (e.date.IsEmpty() == FALSE)
		{
			entries.push_back(e);
		}
	}
	return entries;
}

void AiUsageRecord(const AiModelConfig &model, bool usageKnown, long long inputTokens, long long outputTokens)
{
	std::vector<AiUsageEntry> entries = AiUsageLoad();
	CString today = AiUsageDate();

	AiUsageEntry *entry = nullptr;
	for (AiUsageEntry &e : entries)
	{
		if (e.date == today && e.modelId == model.id)
		{
			entry = &e;
			break;
		}
	}
	if (entry == nullptr)
	{
		entries.push_back(AiUsageEntry());
		entry = &entries.back();
		entry->date = today;
		entry->modelId = model.id;
	}

	entry->modelName = model.name;
	entry->model = model.model;
	entry->requests++;
	if (usageKnown)
	{
		entry->inputTokens += inputTokens;
		entry->outputTokens += outputTokens;
	}
	else
	{
		entry->unknownRequests++;
	}

	CString error;
	if (!SaveEntries(entries, &error))
	{
		Log(_T("AI: ") + error);
	}
}

bool AiUsageReset(CString *error)
{
	return SaveEntries(std::vector<AiUsageEntry>(), error);
}
