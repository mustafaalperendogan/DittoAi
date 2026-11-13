#pragma once

#include "AiConfig.h"

// Local retrieval for the AI module: the files/folders attached to a model are split
// into chunks and searched with BM25 keyword scoring. Works offline and with every
// provider. Indexes are kept in memory and rebuilt when files change.

struct AiRagStats
{
	int files = 0;
	int chunks = 0;
	int skippedFiles = 0;
	CString error;
};

// Returns the most relevant chunks formatted for the system prompt, or "" if none.
// Thread safe; may take a while the first time (index build).
CString AiRagRetrieve(const AiModelConfig &model, const CString &query, AiRagStats *stats = nullptr);

// Builds (or refreshes) the index now and reports what was found.
AiRagStats AiRagBuildIndex(const AiModelConfig &model);

// Extensions that are indexed.
bool AiRagIsSupportedFile(const CString &path);
