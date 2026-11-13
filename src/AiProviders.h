#pragma once

#include <string>
#include "AiConfig.h"

// Calls a configured AI model. Supports the Anthropic Messages API, the OpenAI Chat
// Completions API (OpenAI and every OpenAI compatible server) and Google Gemini.
// All functions here are blocking and meant to run on a worker thread.

struct AiImage
{
	std::string base64;		// PNG data, base64 encoded
	CString mediaType = _T("image/png");
	int width = 0;
	int height = 0;

	bool IsEmpty() const { return base64.empty(); }
};

struct AiCallInput
{
	AiModelConfig model;
	CString apiKey;
	CString instruction;
	CString text;			// copied text (empty for images)
	AiImage image;			// copied image (empty for text)
	CString ragContext;		// retrieved knowledge, already formatted
};

struct AiCallResult
{
	bool ok = false;
	CString text;
	CString error;
	CString warning;		// e.g. answer cut off at max tokens

	bool usageKnown = false;		// the provider reported token counts
	long long inputTokens = 0;		// prompt, context and copied content
	long long outputTokens = 0;		// answer, including thinking/reasoning tokens
};

AiCallResult AiCallModel(const AiCallInput &input);

// Sends a tiny request to check URL, key and model name.
AiCallResult AiTestConnection(const AiModelConfig &model);
