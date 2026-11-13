#pragma once

#include <string>
#include <vector>
#include <utility>

// Small JSON reader/writer used by the AI module (config file and HTTP APIs).

std::string AiToUtf8(const CString &text);
CString AiFromUtf8(const std::string &text);

struct AiJson
{
	enum Type { Null, Bool, Number, String, Array, Object };

	Type type = Null;
	bool boolValue = false;
	double number = 0;
	std::string str;
	std::vector<AiJson> items;
	std::vector<std::pair<std::string, AiJson>> members;

	static bool Parse(const std::string &text, AiJson &out);

	const AiJson *Get(const char *key) const;
	std::string GetString(const char *key, const std::string &def = std::string()) const;
	CString GetCString(const char *key, const CString &def = CString()) const;
	double GetNumber(const char *key, double def) const;
	bool GetBool(const char *key, bool def) const;
	const std::vector<AiJson> &GetArray(const char *key) const;
};

class AiJsonWriter
{
public:
	AiJsonWriter();

	void BeginObject(const char *key = nullptr);
	void EndObject();
	void BeginArray(const char *key = nullptr);
	void EndArray();

	void Str(const char *key, const CString &value);
	void StrUtf8(const char *key, const std::string &utf8);
	void Num(const char *key, double value);
	void Int(const char *key, long long value);
	void Bool(const char *key, bool value);

	const std::string &Result() const { return m_out; }

	static std::string Quote(const std::string &utf8);

private:
	void Prefix(const char *key);

	std::string m_out;
	std::vector<bool> m_first;
};
