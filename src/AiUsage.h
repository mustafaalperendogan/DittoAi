#pragma once

#include <vector>
#include "AiConfig.h"

// Token usage of the AI module. Summed per day and model in AiUsage.json next to
// AiConfig.json; written on the UI thread after every answered request.

struct AiUsageEntry
{
	CString date;				// local date, YYYY-MM-DD
	CString modelId;
	CString modelName;			// name at the time of the request (the model may be renamed or removed later)
	CString model;				// provider's model id
	int requests = 0;
	int unknownRequests = 0;	// answers that did not report token counts (some local servers)
	long long inputTokens = 0;
	long long outputTokens = 0;
};

void AiUsageRecord(const AiModelConfig &model, bool usageKnown, long long inputTokens, long long outputTokens);
std::vector<AiUsageEntry> AiUsageLoad();
bool AiUsageReset(CString *error = nullptr);

// Local date as YYYY-MM-DD, daysAgo days before today.
CString AiUsageDate(int daysAgo = 0);
CString AiUsageFilePath();
