#include "stdafx.h"
#include "AiJson.h"
#include <locale.h>

std::string AiToUtf8(const CString &text)
{
	if (text.IsEmpty())
	{
		return std::string();
	}
	int len = ::WideCharToMultiByte(CP_UTF8, 0, text, text.GetLength(), NULL, 0, NULL, NULL);
	std::string out(len, '\0');
	::WideCharToMultiByte(CP_UTF8, 0, text, text.GetLength(), &out[0], len, NULL, NULL);
	return out;
}

CString AiFromUtf8(const std::string &text)
{
	if (text.empty())
	{
		return CString();
	}
	int len = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), NULL, 0);
	CString out;
	::MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.GetBuffer(len), len);
	out.ReleaseBuffer(len);
	return out;
}

// ---------------------------------------------------------------------------
// Reader

namespace
{
	// Numbers in JSON always use '.', independent of the user's locale.
	_locale_t CLocale()
	{
		static _locale_t locale = _create_locale(LC_NUMERIC, "C");
		return locale;
	}

	class Parser
	{
	public:
		explicit Parser(const std::string &text) : m_s(text), m_pos(0) {}

		bool Parse(AiJson &out)
		{
			SkipWs();
			if (!ParseValue(out, 0)) return false;
			SkipWs();
			return m_pos == m_s.size();
		}

	private:
		const std::string &m_s;
		size_t m_pos;

		void SkipWs()
		{
			while (m_pos < m_s.size() && (m_s[m_pos] == ' ' || m_s[m_pos] == '\n' || m_s[m_pos] == '\r' || m_s[m_pos] == '\t'))
				m_pos++;
		}

		bool Match(const char *word)
		{
			size_t len = strlen(word);
			if (m_s.compare(m_pos, len, word) != 0) return false;
			m_pos += len;
			return true;
		}

		static void AppendUtf8(std::string &out, unsigned int cp)
		{
			if (cp < 0x80) out += (char)cp;
			else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
			else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
			else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
		}

		bool ParseHex4(unsigned int &cp)
		{
			if (m_pos + 4 > m_s.size()) return false;
			cp = 0;
			for (int i = 0; i < 4; i++)
			{
				char c = m_s[m_pos++];
				cp <<= 4;
				if (c >= '0' && c <= '9') cp |= c - '0';
				else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
				else return false;
			}
			return true;
		}

		bool ParseString(std::string &out)
		{
			if (m_pos >= m_s.size() || m_s[m_pos] != '"') return false;
			m_pos++;
			while (m_pos < m_s.size())
			{
				char c = m_s[m_pos++];
				if (c == '"') return true;
				if (c != '\\') { out += c; continue; }
				if (m_pos >= m_s.size()) return false;
				char e = m_s[m_pos++];
				switch (e)
				{
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u':
				{
					unsigned int cp;
					if (!ParseHex4(cp)) return false;
					if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 6 <= m_s.size() && m_s[m_pos] == '\\' && m_s[m_pos + 1] == 'u')
					{
						size_t save = m_pos;
						m_pos += 2;
						unsigned int low;
						if (ParseHex4(low) && low >= 0xDC00 && low <= 0xDFFF)
						{
							cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
						}
						else
						{
							m_pos = save;
						}
					}
					AppendUtf8(out, cp);
					break;
				}
				default: return false;
				}
			}
			return false;
		}

		bool ParseValue(AiJson &v, int depth)
		{
			if (depth > 64 || m_pos >= m_s.size()) return false;
			char c = m_s[m_pos];
			if (c == '{')
			{
				v.type = AiJson::Object;
				m_pos++;
				SkipWs();
				if (m_pos < m_s.size() && m_s[m_pos] == '}') { m_pos++; return true; }
				while (true)
				{
					SkipWs();
					std::string key;
					if (!ParseString(key)) return false;
					SkipWs();
					if (m_pos >= m_s.size() || m_s[m_pos] != ':') return false;
					m_pos++;
					SkipWs();
					AiJson child;
					if (!ParseValue(child, depth + 1)) return false;
					v.members.emplace_back(std::move(key), std::move(child));
					SkipWs();
					if (m_pos < m_s.size() && m_s[m_pos] == ',') { m_pos++; continue; }
					if (m_pos < m_s.size() && m_s[m_pos] == '}') { m_pos++; return true; }
					return false;
				}
			}
			if (c == '[')
			{
				v.type = AiJson::Array;
				m_pos++;
				SkipWs();
				if (m_pos < m_s.size() && m_s[m_pos] == ']') { m_pos++; return true; }
				while (true)
				{
					SkipWs();
					AiJson child;
					if (!ParseValue(child, depth + 1)) return false;
					v.items.push_back(std::move(child));
					SkipWs();
					if (m_pos < m_s.size() && m_s[m_pos] == ',') { m_pos++; continue; }
					if (m_pos < m_s.size() && m_s[m_pos] == ']') { m_pos++; return true; }
					return false;
				}
			}
			if (c == '"')
			{
				v.type = AiJson::String;
				return ParseString(v.str);
			}
			if (Match("true")) { v.type = AiJson::Bool; v.boolValue = true; return true; }
			if (Match("false")) { v.type = AiJson::Bool; v.boolValue = false; return true; }
			if (Match("null")) { v.type = AiJson::Null; return true; }

			size_t start = m_pos;
			while (m_pos < m_s.size() && strchr("+-0123456789.eE", m_s[m_pos]) != nullptr) m_pos++;
			if (m_pos == start) return false;
			v.type = AiJson::Number;
			v.number = _atof_l(m_s.substr(start, m_pos - start).c_str(), CLocale());
			return true;
		}
	};
}

bool AiJson::Parse(const std::string &text, AiJson &out)
{
	out = AiJson();
	size_t start = 0;
	if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
	{
		start = 3;
	}
	std::string body = start ? text.substr(start) : std::string();
	return Parser(start ? body : text).Parse(out);
}

const AiJson *AiJson::Get(const char *key) const
{
	if (type != Object) return nullptr;
	for (auto &m : members)
	{
		if (m.first == key) return &m.second;
	}
	return nullptr;
}

std::string AiJson::GetString(const char *key, const std::string &def) const
{
	const AiJson *v = Get(key);
	return (v && v->type == String) ? v->str : def;
}

CString AiJson::GetCString(const char *key, const CString &def) const
{
	const AiJson *v = Get(key);
	return (v && v->type == String) ? AiFromUtf8(v->str) : def;
}

double AiJson::GetNumber(const char *key, double def) const
{
	const AiJson *v = Get(key);
	return (v && v->type == Number) ? v->number : def;
}

bool AiJson::GetBool(const char *key, bool def) const
{
	const AiJson *v = Get(key);
	return (v && v->type == Bool) ? v->boolValue : def;
}

const std::vector<AiJson> &AiJson::GetArray(const char *key) const
{
	static const std::vector<AiJson> empty;
	const AiJson *v = Get(key);
	return (v && v->type == Array) ? v->items : empty;
}

// ---------------------------------------------------------------------------
// Writer

AiJsonWriter::AiJsonWriter()
{
	m_out.reserve(1024);
}

std::string AiJsonWriter::Quote(const std::string &utf8)
{
	std::string out;
	out.reserve(utf8.size() + 16);
	out += '"';
	for (unsigned char c : utf8)
	{
		switch (c)
		{
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20)
			{
				char buf[8];
				sprintf_s(buf, "\\u%04x", c);
				out += buf;
			}
			else
			{
				out += (char)c;
			}
		}
	}
	out += '"';
	return out;
}

void AiJsonWriter::Prefix(const char *key)
{
	if (m_first.empty() == false)
	{
		if (m_first.back())
		{
			m_first.back() = false;
		}
		else
		{
			m_out += ',';
		}
	}
	if (key != nullptr)
	{
		m_out += Quote(key);
		m_out += ':';
	}
}

void AiJsonWriter::BeginObject(const char *key)
{
	Prefix(key);
	m_out += '{';
	m_first.push_back(true);
}

void AiJsonWriter::EndObject()
{
	m_out += '}';
	m_first.pop_back();
}

void AiJsonWriter::BeginArray(const char *key)
{
	Prefix(key);
	m_out += '[';
	m_first.push_back(true);
}

void AiJsonWriter::EndArray()
{
	m_out += ']';
	m_first.pop_back();
}

void AiJsonWriter::Str(const char *key, const CString &value)
{
	Prefix(key);
	m_out += Quote(AiToUtf8(value));
}

void AiJsonWriter::StrUtf8(const char *key, const std::string &utf8)
{
	Prefix(key);
	m_out += Quote(utf8);
}

void AiJsonWriter::Num(const char *key, double value)
{
	Prefix(key);
	char buf[64];
	_sprintf_s_l(buf, sizeof(buf), "%.6g", CLocale(), value);
	m_out += buf;
}

void AiJsonWriter::Int(const char *key, long long value)
{
	Prefix(key);
	m_out += std::to_string(value);
}

void AiJsonWriter::Bool(const char *key, bool value)
{
	Prefix(key);
	m_out += value ? "true" : "false";
}
