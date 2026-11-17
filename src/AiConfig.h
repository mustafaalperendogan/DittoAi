#pragma once

#include <vector>

// Configuration of the AI module, stored as JSON next to Ditto's other data
// (AiConfig.json). API keys are encrypted with Windows DPAPI for the current user.

enum class AiProvider
{
	Anthropic = 0,
	OpenAI = 1,
	OpenAICompatible = 2,	// Ollama, LM Studio, OpenRouter, Groq, Azure OpenAI v1, vLLM...
	Gemini = 3,
	Count
};

enum class AiContentType
{
	Text = 0,
	Image = 1,
	Both = 2
};

struct AiExample
{
	CString input;
	CString output;
};

struct AiModelConfig
{
	CString id;
	CString name;

	// connection
	AiProvider provider = AiProvider::Anthropic;
	CString baseUrl;			// empty = provider default
	CString apiKey;				// empty = provider environment variable
	CString authHeader;			// OpenAI compatible only: empty = "Authorization: Bearer", otherwise header name (e.g. api-key)
	CString model;
	bool supportsImages = true;

	// behaviour ("fine tuning")
	CString systemPrompt;
	double temperature = -1;	// < 0 = provider default
	int maxTokens = 8192;
	CString effort;				// Anthropic only: "", low, medium, high, xhigh, max
	std::vector<AiExample> examples;

	// RAG
	bool ragEnabled = false;
	std::vector<CString> ragSources;	// files or folders
	int ragTopK = 4;
	int ragChunkChars = 1200;

	CString EffectiveBaseUrl() const;
	CString EnvironmentKeyName() const;
	CString ResolveApiKey() const;
	bool RequiresApiKey() const { return provider != AiProvider::OpenAICompatible; }

	static CString ProviderName(AiProvider provider);
	static CString DefaultBaseUrl(AiProvider provider);
	static CString DefaultModel(AiProvider provider);

	static CString ExamplesToText(const std::vector<AiExample> &examples);
	static std::vector<AiExample> ExamplesFromText(const CString &text);
};

struct AiAction
{
	CString id;
	CString name;
	CString instruction;
	AiContentType appliesTo = AiContentType::Text;
	CString modelId;			// empty = default model for the content type

	bool AppliesTo(bool image) const
	{
		return appliesTo == AiContentType::Both || appliesTo == (image ? AiContentType::Image : AiContentType::Text);
	}
};

struct AiNotifySettings
{
	bool showProgress = true;
	bool showSuccess = true;
	bool showErrors = true;
	int position = 0;			// 0 bottom right, 1 top right, 2 bottom left, 3 top left
	int durationSec = 6;
	bool sound = false;
	bool showPreview = true;
};

struct AiConfig
{
	bool enabled = true;
	int doubleCopyMs = 400;

	std::vector<AiModelConfig> models;
	std::vector<AiAction> actions;
	CString defaultTextModelId;
	CString defaultImageModelId;

	AiNotifySettings notify;

	void SetDefaults();
	bool Load();				// returns false and keeps defaults if the file is missing or invalid
	bool Save(CString *error = nullptr) const;

	const AiModelConfig *FindModel(const CString &id) const;
	const AiModelConfig *DefaultModelFor(bool image) const;
	const AiModelConfig *ResolveModel(const AiAction &action, bool image) const;

	// Clears references to a removed model.
	void OnModelRemoved(const CString &id);

	static CString FilePath();
	static CString NewId();
};

// Secrets stored in settings: encrypted with DPAPI for the current Windows user ("dpapi:<base64>").
CString AiProtectSecret(const CString &plain);
CString AiUnprotectSecret(const CString &stored);
