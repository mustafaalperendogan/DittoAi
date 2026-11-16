#include "stdafx.h"
#include "AiDialogs.h"
#include "AiProviders.h"
#include "AiRag.h"
#include "AiUsage.h"

#include <algorithm>
#include <memory>
#include <locale.h>
#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")

namespace
{
	const WORD IDC_AI_TAB = 1000;

	const UINT WM_AI_TEST_DONE = WM_APP + 40;
	const UINT WM_AI_RAG_DONE = WM_APP + 41;

	LPCTSTR EFFORT_VALUES[] = { _T("low"), _T("medium"), _T("high"), _T("xhigh"), _T("max") };
	LPCTSTR POSITION_NAMES[] = { _T("Sağ alt"), _T("Sağ üst"), _T("Sol alt"), _T("Sol üst") };
	LPCTSTR CONTENT_TYPE_NAMES[] = { _T("Metin"), _T("Görsel"), _T("Metin ve görsel") };

	CString ContentTypeName(AiContentType type)
	{
		int i = (int)type;
		return (i >= 0 && i < _countof(CONTENT_TYPE_NAMES)) ? CONTENT_TYPE_NAMES[i] : CString();
	}

	CString GetText(CWnd *pDlg, int id)
	{
		CString text;
		pDlg->GetDlgItemText(id, text);
		return text;
	}

	CString ToCrLf(CString text)
	{
		text.Replace(_T("\r\n"), _T("\n"));
		text.Replace(_T("\n"), _T("\r\n"));
		return text;
	}

	// Fills a combo with the models; item data = index into config.models, -1 = "default" entry.
	void FillModelCombo(CComboBox *pCombo, const AiConfig &config, const CString &selectedId, LPCTSTR defaultEntry, bool imagesOnly)
	{
		pCombo->ResetContent();
		int select = -1;
		if (defaultEntry != NULL)
		{
			int i = pCombo->AddString(defaultEntry);
			pCombo->SetItemData(i, (DWORD_PTR)-1);
			select = i;
		}
		for (int m = 0; m < (int)config.models.size(); m++)
		{
			const AiModelConfig &model = config.models[m];
			if (imagesOnly && !model.supportsImages)
			{
				continue;
			}
			int i = pCombo->AddString(model.name + _T("  (") + model.model + _T(")"));
			pCombo->SetItemData(i, (DWORD_PTR)m);
			if (model.id == selectedId)
			{
				select = i;
			}
		}
		if (select < 0 && pCombo->GetCount() > 0)
		{
			select = 0;
		}
		pCombo->SetCurSel(select);
	}

	CString SelectedModelId(CComboBox *pCombo, const AiConfig &config)
	{
		int sel = pCombo->GetCurSel();
		if (sel < 0)
		{
			return CString();
		}
		int index = (int)(INT_PTR)pCombo->GetItemData(sel);
		return (index >= 0 && index < (int)config.models.size()) ? config.models[index].id : CString();
	}

	bool ParseInt(CWnd *pDlg, int id, int minValue, int maxValue, int &value, LPCTSTR fieldName)
	{
		CString text = GetText(pDlg, id);
		text.Trim();
		int parsed = _ttoi(text);
		if (text.IsEmpty() || parsed < minValue || parsed > maxValue)
		{
			CString msg;
			msg.Format(_T("%s %d ile %d arasında olmalı."), fieldName, minValue, maxValue);
			pDlg->MessageBox(msg, _T("Ditto AI"), MB_ICONWARNING);
			pDlg->GetDlgItem(id)->SetFocus();
			return false;
		}
		value = parsed;
		return true;
	}
}

// ===========================================================================
// CAiDialogTemplate

void CAiDialogTemplate::AlignDword()
{
	if (m_data.size() % 2 != 0)
	{
		m_data.push_back(0);
	}
}

void CAiDialogTemplate::AddString(LPCWSTR s)
{
	while (*s)
	{
		m_data.push_back(*s++);
	}
	m_data.push_back(0);
}

void CAiDialogTemplate::Header(DWORD style, DWORD exStyle, LPCWSTR title, short cx, short cy)
{
	m_data.clear();
	AddDword(style);
	AddDword(exStyle);
	m_countIndex = m_data.size();
	AddWord(0);		// control count, incremented by Control()
	AddWord(0);
	AddWord(0);
	AddWord((WORD)cx);
	AddWord((WORD)cy);
	AddWord(0);		// menu
	AddWord(0);		// class
	AddString(title);
	AddWord(9);		// font size
	AddString(L"Segoe UI");
}

void CAiDialogTemplate::Begin(LPCWSTR title, short cx, short cy)
{
	Header(DS_SETFONT | DS_SHELLFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, title, cx, cy);
}

void CAiDialogTemplate::BeginChild(short cx, short cy)
{
	Header(DS_SETFONT | DS_SHELLFONT | DS_CONTROL | WS_CHILD, WS_EX_CONTROLPARENT, L"", cx, cy);
}

void CAiDialogTemplate::Control(WORD classAtom, LPCWSTR text, WORD id, DWORD style, DWORD exStyle, short x, short y, short cx, short cy)
{
	AlignDword();
	AddDword(style | WS_CHILD | WS_VISIBLE);
	AddDword(exStyle);
	AddWord((WORD)x);
	AddWord((WORD)y);
	AddWord((WORD)cx);
	AddWord((WORD)cy);
	AddWord(id);
	AddWord(0xFFFF);
	AddWord(classAtom);
	AddString(text);
	AddWord(0);
	m_data[m_countIndex]++;
}

void CAiDialogTemplate::Control(LPCWSTR className, LPCWSTR text, WORD id, DWORD style, DWORD exStyle, short x, short y, short cx, short cy)
{
	AlignDword();
	AddDword(style | WS_CHILD | WS_VISIBLE);
	AddDword(exStyle);
	AddWord((WORD)x);
	AddWord((WORD)y);
	AddWord((WORD)cx);
	AddWord((WORD)cy);
	AddWord(id);
	AddString(className);
	AddString(text);
	AddWord(0);
	m_data[m_countIndex]++;
}

void CAiDialogTemplate::Label(LPCWSTR text, short x, short y, short cx, short cy, WORD id)
{
	Control((WORD)0x0082, text, id, SS_LEFT, 0, x, y, cx, cy);
}

void CAiDialogTemplate::Edit(WORD id, short x, short y, short cx, short cy, DWORD extraStyle)
{
	Control((WORD)0x0081, L"", id, WS_TABSTOP | ES_AUTOHSCROLL | extraStyle, WS_EX_CLIENTEDGE, x, y, cx, cy);
}

void CAiDialogTemplate::MultiLineEdit(WORD id, short x, short y, short cx, short cy, bool wantReturn)
{
	Control((WORD)0x0081, L"", id, WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | (wantReturn ? ES_WANTRETURN : 0), WS_EX_CLIENTEDGE, x, y, cx, cy);
}

void CAiDialogTemplate::Button(WORD id, LPCWSTR text, short x, short y, short cx, short cy, bool isDefault)
{
	Control((WORD)0x0080, text, id, WS_TABSTOP | (isDefault ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), 0, x, y, cx, cy);
}

void CAiDialogTemplate::Check(WORD id, LPCWSTR text, short x, short y, short cx, short cy)
{
	Control((WORD)0x0080, text, id, WS_TABSTOP | BS_AUTOCHECKBOX, 0, x, y, cx, cy);
}

void CAiDialogTemplate::Combo(WORD id, short x, short y, short cx, short dropHeight)
{
	Control((WORD)0x0085, L"", id, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, x, y, cx, dropHeight);
}

void CAiDialogTemplate::ListBox(WORD id, short x, short y, short cx, short cy)
{
	Control((WORD)0x0083, L"", id, WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, WS_EX_CLIENTEDGE, x, y, cx, cy);
}

void CAiDialogTemplate::ListView(WORD id, short x, short y, short cx, short cy)
{
	Control(L"SysListView32", L"", id, WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE, x, y, cx, cy);
}

// ===========================================================================
// CAiPage / CAiTabbedDialog

bool CAiPage::CreatePage(CWnd *pParent)
{
	return CreateIndirect(m_template.Get(), pParent) != FALSE;
}

BOOL CAiPage::OnInitDialog()
{
	CDialog::OnInitDialog();
	::EnableThemeDialogTexture(m_hWnd, ETDT_ENABLETAB);
	Load();
	return FALSE;
}

BEGIN_MESSAGE_MAP(CAiTabbedDialog, CDialog)
	ON_NOTIFY(TCN_SELCHANGE, IDC_AI_TAB, OnTabChanged)
END_MESSAGE_MAP()

CAiTabbedDialog::CAiTabbedDialog(LPCWSTR title, short cx, short cy, CWnd *pParent)
{
	m_template.Begin(title, cx, cy);
	m_template.Control(L"SysTabControl32", L"", IDC_AI_TAB, WS_TABSTOP | WS_CLIPSIBLINGS, 0, 7, 7, cx - 14, cy - 34);
	m_template.Button(IDOK, L"Kaydet", cx - 114, cy - 21, 50, 14, true);
	m_template.Button(IDCANCEL, L"İptal", cx - 57, cy - 21, 50, 14);
	InitModalIndirect(m_template.Get(), pParent);
}

void CAiTabbedDialog::AddPage(LPCTSTR title, CAiPage *pPage)
{
	m_pages.push_back({ title, pPage });
}

BOOL CAiTabbedDialog::OnInitDialog()
{
	CDialog::OnInitDialog();

	m_tab.SubclassDlgItem(IDC_AI_TAB, this);

	CRect area;
	m_tab.GetWindowRect(area);
	ScreenToClient(area);
	for (size_t i = 0; i < m_pages.size(); i++)
	{
		m_tab.InsertItem((int)i, m_pages[i].first);
	}
	m_tab.AdjustRect(FALSE, area);

	for (auto &page : m_pages)
	{
		page.second->CreatePage(this);
		page.second->SetWindowPos(&m_tab, area.left, area.top, area.Width(), area.Height(), SWP_NOACTIVATE);
		page.second->ShowWindow(SW_HIDE);
	}

	SelectPage(m_startPage);
	::SetForegroundWindow(m_hWnd);
	return TRUE;
}

void CAiTabbedDialog::SelectPage(int index)
{
	if (m_tab.m_hWnd == NULL)
	{
		m_startPage = index;
		return;
	}
	if (index < 0 || index >= (int)m_pages.size())
	{
		index = 0;
	}
	m_tab.SetCurSel(index);
	for (int i = 0; i < (int)m_pages.size(); i++)
	{
		m_pages[i].second->ShowWindow(i == index ? SW_SHOW : SW_HIDE);
	}
	m_pages[index].second->OnPageShown();
}

void CAiTabbedDialog::OnTabChanged(NMHDR *pNMHDR, LRESULT *pResult)
{
	SelectPage(m_tab.GetCurSel());
	*pResult = 0;
}

void CAiTabbedDialog::OnOK()
{
	for (int i = 0; i < (int)m_pages.size(); i++)
	{
		if (!m_pages[i].second->Save())
		{
			SelectPage(i);
			return;
		}
	}
	CDialog::OnOK();
}

// ===========================================================================
// Model editor pages

namespace
{
	enum
	{
		IDC_M_NAME = 3001, IDC_M_PROVIDER, IDC_M_URL, IDC_M_URL_HINT, IDC_M_KEY, IDC_M_AUTH, IDC_M_MODEL, IDC_M_IMAGES,
		IDC_M_TEST, IDC_M_AUTH_HINT, IDC_M_MODEL_HINT, IDC_M_STATUS, IDC_M_KEY_HINT,

		IDC_T_SYSTEM = 3101, IDC_T_TEMP, IDC_T_MAXTOK, IDC_T_EFFORT, IDC_T_EXAMPLES,

		IDC_R_ENABLED = 3201, IDC_R_SOURCES, IDC_R_ADD_FOLDER, IDC_R_ADD_FILE, IDC_R_REMOVE, IDC_R_TOPK, IDC_R_CHUNK, IDC_R_TEST, IDC_R_STATUS,
	};

	struct TestJob
	{
		HWND notify;
		AiModelConfig model;
	};

	UINT TestThread(LPVOID param)
	{
		std::unique_ptr<TestJob> job((TestJob *)param);
		AiCallResult *result = new AiCallResult(AiTestConnection(job->model));
		if (!::PostMessage(job->notify, WM_AI_TEST_DONE, 0, (LPARAM)result))
		{
			delete result;
		}
		return 0;
	}

	UINT RagThread(LPVOID param)
	{
		std::unique_ptr<TestJob> job((TestJob *)param);
		AiRagStats *stats = new AiRagStats(AiRagBuildIndex(job->model));
		if (!::PostMessage(job->notify, WM_AI_RAG_DONE, 0, (LPARAM)stats))
		{
			delete stats;
		}
		return 0;
	}

	// ---------------------------------------------------------------- connection

	class CConnectionPage : public CAiPage
	{
	public:
		CConnectionPage(AiModelConfig &model) : m_model(model), m_lastProvider(-1)
		{
			m_template.BeginChild(350, 240);
			short lx = 7, fx = 100, fw = 243;
			m_template.Label(L"Görünen ad:", lx, 8, 90);
			m_template.Edit(IDC_M_NAME, fx, 6, fw);
			m_template.Label(L"Sağlayıcı:", lx, 26, 90);
			m_template.Combo(IDC_M_PROVIDER, fx, 24, fw);
			m_template.Label(L"Temel URL:", lx, 44, 90);
			m_template.Edit(IDC_M_URL, fx, 42, fw);
			m_template.Label(L"", fx, 56, fw, 8, IDC_M_URL_HINT);
			m_template.Label(L"API anahtarı:", lx, 70, 90);
			m_template.Edit(IDC_M_KEY, fx, 68, fw, 12, ES_PASSWORD);
			m_template.Label(L"", fx, 82, fw, 8, IDC_M_KEY_HINT);
			m_template.Label(L"Anahtar başlığı:", lx, 96, 90);
			m_template.Edit(IDC_M_AUTH, fx, 94, 100);
			m_template.Label(L"boş = Authorization: Bearer (Azure için: api-key)", 205, 96, 140, 8, IDC_M_AUTH_HINT);
			m_template.Label(L"Model kimliği:", lx, 114, 90);
			m_template.Edit(IDC_M_MODEL, fx, 112, fw);
			m_template.Label(L"Örn: claude-opus-5-5 · gpt-... · gemini-... · llama3.2 · qwen2.5", fx, 126, fw, 8, IDC_M_MODEL_HINT);
			m_template.Check(IDC_M_IMAGES, L"Görsel girdi destekler (görsel eylemlerinde kullanılabilir)", fx, 140, fw);
			m_template.Button(IDC_M_TEST, L"Bağlantıyı test et", fx, 158, 90, 14);
			m_template.Label(L"", fx, 176, fw, 56, IDC_M_STATUS);
		}

		void Load() override
		{
			CComboBox *pProvider = (CComboBox *)GetDlgItem(IDC_M_PROVIDER);
			for (int p = 0; p < (int)AiProvider::Count; p++)
			{
				pProvider->AddString(AiModelConfig::ProviderName((AiProvider)p));
			}
			pProvider->SetCurSel((int)m_model.provider);
			m_lastProvider = (int)m_model.provider;

			SetDlgItemText(IDC_M_NAME, m_model.name);
			SetDlgItemText(IDC_M_URL, m_model.baseUrl);
			SetDlgItemText(IDC_M_KEY, m_model.apiKey);
			SetDlgItemText(IDC_M_AUTH, m_model.authHeader);
			SetDlgItemText(IDC_M_MODEL, m_model.model);
			CheckDlgButton(IDC_M_IMAGES, m_model.supportsImages ? BST_CHECKED : BST_UNCHECKED);
			UpdateHints();
		}

		bool Save() override
		{
			Collect(m_model);
			if (m_model.model.IsEmpty())
			{
				MessageBox(_T("Model kimliğini girin (sağlayıcıdaki modelin adı)."), _T("Ditto AI"), MB_ICONWARNING);
				GetDlgItem(IDC_M_MODEL)->SetFocus();
				return false;
			}
			if (m_model.name.IsEmpty())
			{
				m_model.name = m_model.model;
			}
			return true;
		}

	protected:
		DECLARE_MESSAGE_MAP()

		void Collect(AiModelConfig &model)
		{
			model.provider = (AiProvider)max(0, ((CComboBox *)GetDlgItem(IDC_M_PROVIDER))->GetCurSel());
			model.name = GetText(this, IDC_M_NAME).Trim();
			model.baseUrl = GetText(this, IDC_M_URL).Trim();
			model.apiKey = GetText(this, IDC_M_KEY).Trim();
			model.authHeader = GetText(this, IDC_M_AUTH).Trim();
			model.model = GetText(this, IDC_M_MODEL).Trim();
			model.supportsImages = IsDlgButtonChecked(IDC_M_IMAGES) == BST_CHECKED;
		}

		void UpdateHints()
		{
			AiModelConfig temp;
			Collect(temp);

			SetDlgItemText(IDC_M_URL_HINT, _T("Boş bırakılırsa: ") + AiModelConfig::DefaultBaseUrl(temp.provider));

			CString keyHint;
			if (temp.provider == AiProvider::OpenAICompatible)
			{
				keyHint = _T("Yerel sunucularda (Ollama, LM Studio) genelde gerekmez.");
			}
			else
			{
				keyHint = _T("Boş bırakılırsa ") + temp.EnvironmentKeyName() + _T(" ortam değişkeni kullanılır.");
			}
			SetDlgItemText(IDC_M_KEY_HINT, keyHint);

			bool compatible = temp.provider == AiProvider::OpenAICompatible;
			GetDlgItem(IDC_M_AUTH)->EnableWindow(compatible);
			GetDlgItem(IDC_M_AUTH_HINT)->EnableWindow(compatible);
		}

		afx_msg void OnProviderChanged()
		{
			int provider = ((CComboBox *)GetDlgItem(IDC_M_PROVIDER))->GetCurSel();
			if (provider != m_lastProvider)
			{
				// replace the URL / model only if they still hold the old provider's defaults
				CString url = GetText(this, IDC_M_URL).Trim();
				if (m_lastProvider >= 0 && url == AiModelConfig::DefaultBaseUrl((AiProvider)m_lastProvider))
				{
					SetDlgItemText(IDC_M_URL, _T(""));
				}
				CString model = GetText(this, IDC_M_MODEL).Trim();
				if (model.IsEmpty() || (m_lastProvider >= 0 && model == AiModelConfig::DefaultModel((AiProvider)m_lastProvider)))
				{
					SetDlgItemText(IDC_M_MODEL, AiModelConfig::DefaultModel((AiProvider)provider));
				}
				m_lastProvider = provider;
			}
			UpdateHints();
		}

		afx_msg void OnTest()
		{
			TestJob *job = new TestJob();
			job->notify = m_hWnd;
			job->model = m_model;	// fine tuning fields from the other tab are not needed for the test
			Collect(job->model);
			if (job->model.model.IsEmpty())
			{
				delete job;
				SetDlgItemText(IDC_M_STATUS, _T("Önce model kimliğini girin."));
				return;
			}

			GetDlgItem(IDC_M_TEST)->EnableWindow(FALSE);
			SetDlgItemText(IDC_M_STATUS, _T("Test ediliyor..."));
			if (AfxBeginThread(TestThread, job) == NULL)
			{
				delete job;
				GetDlgItem(IDC_M_TEST)->EnableWindow(TRUE);
			}
		}

		afx_msg LRESULT OnTestDone(WPARAM, LPARAM lParam)
		{
			std::unique_ptr<AiCallResult> result((AiCallResult *)lParam);
			GetDlgItem(IDC_M_TEST)->EnableWindow(TRUE);
			if (result->ok)
			{
				CString answer = result->text.Left(80);
				answer.Replace(_T("\n"), _T(" "));
				SetDlgItemText(IDC_M_STATUS, _T("Bağlantı başarılı. Model cevabı: ") + answer);
			}
			else
			{
				SetDlgItemText(IDC_M_STATUS, _T("Başarısız: ") + result->error);
			}
			return 0;
		}

		AiModelConfig &m_model;
		int m_lastProvider;
	};

	BEGIN_MESSAGE_MAP(CConnectionPage, CAiPage)
		ON_CBN_SELCHANGE(IDC_M_PROVIDER, OnProviderChanged)
		ON_BN_CLICKED(IDC_M_TEST, OnTest)
		ON_MESSAGE(WM_AI_TEST_DONE, OnTestDone)
	END_MESSAGE_MAP()

	// ---------------------------------------------------------------- fine tuning

	class CTuningPage : public CAiPage
	{
	public:
		CTuningPage(AiModelConfig &model) : m_model(model)
		{
			m_template.BeginChild(350, 240);
			m_template.Label(L"Sistem istemi - bu modele her istekte verilecek rol, ton ve kurallar:", 7, 6, 340);
			m_template.MultiLineEdit(IDC_T_SYSTEM, 7, 16, 340, 58);
			m_template.Label(L"Sıcaklık (0-2, boş = varsayılan):", 7, 82, 140);
			m_template.Edit(IDC_T_TEMP, 150, 80, 40);
			m_template.Label(L"Claude'da kullanılmaz.", 196, 82, 150);
			m_template.Label(L"En fazla çıktı token'ı:", 7, 100, 140);
			m_template.Edit(IDC_T_MAXTOK, 150, 98, 50, 12, ES_NUMBER);
			m_template.Label(L"Efor (yalnız Claude):", 7, 118, 140);
			m_template.Combo(IDC_T_EFFORT, 150, 116, 100);
			m_template.Label(L"Örnek girdi/çıktı çiftleri (few-shot). Her örnek: '>>>' satırından sonra girdi, '<<<' satırından sonra beklenen çıktı.", 7, 136, 340, 16);
			m_template.MultiLineEdit(IDC_T_EXAMPLES, 7, 154, 340, 82);
		}

		void Load() override
		{
			SetDlgItemText(IDC_T_SYSTEM, ToCrLf(m_model.systemPrompt));
			if (m_model.temperature >= 0)
			{
				CString t;
				t.Format(_T("%g"), m_model.temperature);
				SetDlgItemText(IDC_T_TEMP, t);
			}
			SetDlgItemInt(IDC_T_MAXTOK, m_model.maxTokens, FALSE);

			CComboBox *pEffort = (CComboBox *)GetDlgItem(IDC_T_EFFORT);
			pEffort->AddString(_T("(model varsayılanı)"));
			int sel = 0;
			for (int i = 0; i < _countof(EFFORT_VALUES); i++)
			{
				pEffort->AddString(EFFORT_VALUES[i]);
				if (m_model.effort.CompareNoCase(EFFORT_VALUES[i]) == 0) sel = i + 1;
			}
			pEffort->SetCurSel(sel);

			SetDlgItemText(IDC_T_EXAMPLES, AiModelConfig::ExamplesToText(m_model.examples));
		}

		bool Save() override
		{
			m_model.systemPrompt = GetText(this, IDC_T_SYSTEM).Trim();

			CString temp = GetText(this, IDC_T_TEMP).Trim();
			temp.Replace(_T(','), _T('.'));
			if (temp.IsEmpty())
			{
				m_model.temperature = -1;
			}
			else
			{
				_locale_t c = _create_locale(LC_NUMERIC, "C");
				double value = _tcstod_l(temp, NULL, c);
				_free_locale(c);
				if (value < 0 || value > 2)
				{
					MessageBox(_T("Sıcaklık 0 ile 2 arasında olmalı (ya da boş)."), _T("Ditto AI"), MB_ICONWARNING);
					GetDlgItem(IDC_T_TEMP)->SetFocus();
					return false;
				}
				m_model.temperature = value;
			}

			if (!ParseInt(this, IDC_T_MAXTOK, 16, 128000, m_model.maxTokens, _T("En fazla çıktı token'ı")))
			{
				return false;
			}

			int sel = ((CComboBox *)GetDlgItem(IDC_T_EFFORT))->GetCurSel();
			m_model.effort = (sel >= 1 && sel <= _countof(EFFORT_VALUES)) ? EFFORT_VALUES[sel - 1] : _T("");

			m_model.examples = AiModelConfig::ExamplesFromText(GetText(this, IDC_T_EXAMPLES));
			return true;
		}

	private:
		AiModelConfig &m_model;
	};

	// ---------------------------------------------------------------- RAG

	class CRagPage : public CAiPage
	{
	public:
		CRagPage(AiModelConfig &model) : m_model(model)
		{
			m_template.BeginChild(350, 240);
			m_template.Check(IDC_R_ENABLED, L"Bu model için bilgi kaynaklarını kullan (RAG)", 7, 6, 340);
			m_template.Label(L"Klasörler ve dosyalar (.txt .md .csv .json .html .xml, kod dosyaları...):", 7, 22, 340);
			m_template.ListBox(IDC_R_SOURCES, 7, 32, 250, 108);
			m_template.Button(IDC_R_ADD_FOLDER, L"Klasör ekle...", 263, 32, 84);
			m_template.Button(IDC_R_ADD_FILE, L"Dosya ekle...", 263, 50, 84);
			m_template.Button(IDC_R_REMOVE, L"Kaldır", 263, 68, 84);
			m_template.Label(L"Getirilecek parça sayısı:", 7, 150, 130);
			m_template.Edit(IDC_R_TOPK, 140, 148, 40, 12, ES_NUMBER);
			m_template.Label(L"Parça boyutu (karakter):", 7, 168, 130);
			m_template.Edit(IDC_R_CHUNK, 140, 166, 50, 12, ES_NUMBER);
			m_template.Button(IDC_R_TEST, L"İndeksi oluştur ve test et", 200, 148, 110, 14);
			m_template.Label(L"", 200, 166, 147, 30, IDC_R_STATUS);
			m_template.Label(L"Kopyalanan içerik ve talimatla en ilgili parçalar anahtar kelime aramasıyla bulunur ve modele bağlam olarak eklenir. "
				L"Dosyalar bu bilgisayarda kalır, yalnızca seçilen parçalar modele gönderilir. Dosyalar değişince indeks kendiliğinden yenilenir.", 7, 202, 340, 34);
		}

		void Load() override
		{
			CheckDlgButton(IDC_R_ENABLED, m_model.ragEnabled ? BST_CHECKED : BST_UNCHECKED);
			CListBox *pList = (CListBox *)GetDlgItem(IDC_R_SOURCES);
			for (const CString &s : m_model.ragSources)
			{
				pList->AddString(s);
			}
			UpdateScroll();
			SetDlgItemInt(IDC_R_TOPK, m_model.ragTopK, FALSE);
			SetDlgItemInt(IDC_R_CHUNK, m_model.ragChunkChars, FALSE);
		}

		bool Save() override
		{
			return Collect(m_model, true);
		}

	protected:
		DECLARE_MESSAGE_MAP()

		bool Collect(AiModelConfig &model, bool validate)
		{
			model.ragEnabled = IsDlgButtonChecked(IDC_R_ENABLED) == BST_CHECKED;
			model.ragSources.clear();
			CListBox *pList = (CListBox *)GetDlgItem(IDC_R_SOURCES);
			for (int i = 0; i < pList->GetCount(); i++)
			{
				CString s;
				pList->GetText(i, s);
				model.ragSources.push_back(s);
			}
			if (!validate)
			{
				model.ragTopK = max(1, (int)GetDlgItemInt(IDC_R_TOPK, NULL, FALSE));
				model.ragChunkChars = max(300, (int)GetDlgItemInt(IDC_R_CHUNK, NULL, FALSE));
				return true;
			}
			return ParseInt(this, IDC_R_TOPK, 1, 20, model.ragTopK, _T("Getirilecek parça sayısı")) &&
				ParseInt(this, IDC_R_CHUNK, 300, 8000, model.ragChunkChars, _T("Parça boyutu"));
		}

		void UpdateScroll()
		{
			// horizontal scroll for long paths
			CListBox *pList = (CListBox *)GetDlgItem(IDC_R_SOURCES);
			CClientDC dc(pList);
			CFont *pOld = dc.SelectObject(pList->GetFont());
			int width = 0;
			for (int i = 0; i < pList->GetCount(); i++)
			{
				CString s;
				pList->GetText(i, s);
				width = max(width, (int)dc.GetTextExtent(s).cx);
			}
			dc.SelectObject(pOld);
			pList->SetHorizontalExtent(width + 10);
		}

		void AddSource(const CString &path)
		{
			CListBox *pList = (CListBox *)GetDlgItem(IDC_R_SOURCES);
			if (pList->FindStringExact(-1, path) == LB_ERR)
			{
				pList->AddString(path);
			}
			CheckDlgButton(IDC_R_ENABLED, BST_CHECKED);
			UpdateScroll();
		}

		afx_msg void OnAddFolder()
		{
			CFolderPickerDialog dlg(NULL, 0, this);
			if (dlg.DoModal() == IDOK)
			{
				AddSource(dlg.GetPathName());
			}
		}

		afx_msg void OnAddFile()
		{
			CFileDialog dlg(TRUE, NULL, NULL, OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_EXPLORER,
				_T("Metin dosyaları|*.txt;*.md;*.markdown;*.csv;*.tsv;*.json;*.xml;*.html;*.htm;*.log;*.ini;*.yaml;*.yml;*.sql|Tüm dosyalar|*.*||"), this);
			std::vector<TCHAR> buffer(64 * 1024, 0);
			dlg.GetOFN().lpstrFile = buffer.data();
			dlg.GetOFN().nMaxFile = (DWORD)buffer.size();
			if (dlg.DoModal() == IDOK)
			{
				POSITION pos = dlg.GetStartPosition();
				while (pos != NULL)
				{
					AddSource(dlg.GetNextPathName(pos));
				}
			}
		}

		afx_msg void OnRemove()
		{
			CListBox *pList = (CListBox *)GetDlgItem(IDC_R_SOURCES);
			int sel = pList->GetCurSel();
			if (sel >= 0)
			{
				pList->DeleteString(sel);
				pList->SetCurSel(min(sel, pList->GetCount() - 1));
				UpdateScroll();
			}
		}

		afx_msg void OnTest()
		{
			TestJob *job = new TestJob();
			job->notify = m_hWnd;
			job->model = m_model;
			Collect(job->model, false);
			GetDlgItem(IDC_R_TEST)->EnableWindow(FALSE);
			SetDlgItemText(IDC_R_STATUS, _T("İndeksleniyor..."));
			if (AfxBeginThread(RagThread, job) == NULL)
			{
				delete job;
				GetDlgItem(IDC_R_TEST)->EnableWindow(TRUE);
			}
		}

		afx_msg LRESULT OnRagDone(WPARAM, LPARAM lParam)
		{
			std::unique_ptr<AiRagStats> stats((AiRagStats *)lParam);
			GetDlgItem(IDC_R_TEST)->EnableWindow(TRUE);
			CString text;
			if (stats->error.IsEmpty() == FALSE)
			{
				text = stats->error;
			}
			else
			{
				text.Format(_T("%d dosya, %d parça indekslendi."), stats->files, stats->chunks);
			}
			if (stats->skippedFiles > 0)
			{
				text.AppendFormat(_T(" %d dosya atlandı (desteklenmeyen tür ya da 4 MB'tan büyük)."), stats->skippedFiles);
			}
			SetDlgItemText(IDC_R_STATUS, text);
			return 0;
		}

		AiModelConfig &m_model;
	};

	BEGIN_MESSAGE_MAP(CRagPage, CAiPage)
		ON_BN_CLICKED(IDC_R_ADD_FOLDER, OnAddFolder)
		ON_BN_CLICKED(IDC_R_ADD_FILE, OnAddFile)
		ON_BN_CLICKED(IDC_R_REMOVE, OnRemove)
		ON_BN_CLICKED(IDC_R_TEST, OnTest)
		ON_MESSAGE(WM_AI_RAG_DONE, OnRagDone)
	END_MESSAGE_MAP()
}

CAiModelDlg::CAiModelDlg(AiModelConfig &model, CWnd *pParent)
	: CAiTabbedDialog(L"Model ayarları", 370, 300, pParent), m_model(model)
{
	AddPage(_T("Bağlantı"), new CConnectionPage(m_model));
	AddPage(_T("İnce ayar"), new CTuningPage(m_model));
	AddPage(_T("RAG (bilgi kaynakları)"), new CRagPage(m_model));
}

CAiModelDlg::~CAiModelDlg()
{
	for (auto &page : m_pages)
	{
		delete page.second;
	}
}

// ===========================================================================
// Action editor

namespace
{
	enum { IDC_A_NAME = 4001, IDC_A_INSTRUCTION, IDC_A_TYPE, IDC_A_MODEL };
}

CAiActionDlg::CAiActionDlg(AiAction &action, const AiConfig &config, CWnd *pParent)
	: m_action(action), m_config(config)
{
	m_template.Begin(L"Eylem", 320, 200);
	m_template.Label(L"Ad:", 7, 8, 60);
	m_template.Edit(IDC_A_NAME, 70, 6, 243);
	m_template.Label(L"Talimat:", 7, 26, 60);
	m_template.MultiLineEdit(IDC_A_INSTRUCTION, 70, 24, 243, 90);
	m_template.Label(L"İçerik türü:", 7, 124, 60);
	m_template.Combo(IDC_A_TYPE, 70, 122, 100, 60);
	m_template.Label(L"Model:", 7, 142, 60);
	m_template.Combo(IDC_A_MODEL, 70, 140, 243, 120);
	m_template.Label(L"Varsayılan seçilirse 'Eylemler ve yönlendirme' sekmesindeki içerik türüne göre varsayılan model kullanılır.", 70, 156, 243, 16);
	m_template.Button(IDOK, L"Tamam", 209, 179, 50, 14, true);
	m_template.Button(IDCANCEL, L"İptal", 263, 179, 50, 14);
	InitModalIndirect(m_template.Get(), pParent);
}

BOOL CAiActionDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	SetDlgItemText(IDC_A_NAME, m_action.name);
	SetDlgItemText(IDC_A_INSTRUCTION, ToCrLf(m_action.instruction));

	CComboBox *pType = (CComboBox *)GetDlgItem(IDC_A_TYPE);
	for (LPCTSTR name : CONTENT_TYPE_NAMES)
	{
		pType->AddString(name);
	}
	pType->SetCurSel((int)m_action.appliesTo);

	FillModelCombo((CComboBox *)GetDlgItem(IDC_A_MODEL), m_config, m_action.modelId, _T("(Varsayılan model)"), false);

	::SetForegroundWindow(m_hWnd);
	return TRUE;
}

void CAiActionDlg::OnOK()
{
	CString name = GetText(this, IDC_A_NAME).Trim();
	CString instruction = GetText(this, IDC_A_INSTRUCTION).Trim();
	if (name.IsEmpty() || instruction.IsEmpty())
	{
		MessageBox(_T("Ad ve talimat boş olamaz."), _T("Ditto AI"), MB_ICONWARNING);
		return;
	}

	m_action.name = name;
	m_action.instruction = instruction;
	m_action.appliesTo = (AiContentType)max(0, ((CComboBox *)GetDlgItem(IDC_A_TYPE))->GetCurSel());
	m_action.modelId = SelectedModelId((CComboBox *)GetDlgItem(IDC_A_MODEL), m_config);
	CDialog::OnOK();
}

// ===========================================================================
// Settings pages

namespace
{
	enum
	{
		IDC_S_MODELS = 2001, IDC_S_MODEL_ADD, IDC_S_MODEL_EDIT, IDC_S_MODEL_COPY, IDC_S_MODEL_DELETE,

		IDC_S_DEF_TEXT = 2101, IDC_S_DEF_IMAGE, IDC_S_ACTIONS, IDC_S_ACTION_ADD, IDC_S_ACTION_EDIT, IDC_S_ACTION_COPY, IDC_S_ACTION_DELETE, IDC_S_ACTION_UP, IDC_S_ACTION_DOWN,

		IDC_G_ENABLED = 2201, IDC_G_INTERVAL, IDC_G_PROGRESS, IDC_G_SUCCESS, IDC_G_ERRORS, IDC_G_PREVIEW, IDC_G_SOUND, IDC_G_POSITION, IDC_G_DURATION, IDC_G_SAMPLE, IDC_G_PATH,

		IDC_U_PERIOD = 2301, IDC_U_LIST, IDC_U_TOTAL, IDC_U_RESET,
	};

	void InitListView(CListCtrl &list, std::initializer_list<std::pair<LPCTSTR, int>> columns)
	{
		list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
		int i = 0;
		CRect rc;
		list.GetClientRect(rc);
		for (auto &c : columns)
		{
			list.InsertColumn(i++, c.first, LVCFMT_LEFT, rc.Width() * c.second / 100);
		}
	}

	int SelectedRow(CListCtrl &list)
	{
		POSITION pos = list.GetFirstSelectedItemPosition();
		return pos ? list.GetNextSelectedItem(pos) : -1;
	}

	// ---------------------------------------------------------------- models

	class CModelsPage : public CAiPage
	{
	public:
		CModelsPage(AiConfig &config) : m_config(config)
		{
			m_template.BeginChild(400, 250);
			m_template.Label(L"Kullanabileceğin yapay zeka modelleri. Her modelin bağlantı, ince ayar (sistem istemi, örnekler) ve RAG (bilgi kaynakları) ayarları ayrıdır.", 7, 6, 386, 16);
			m_template.ListView(IDC_S_MODELS, 7, 26, 312, 214);
			m_template.Button(IDC_S_MODEL_ADD, L"Ekle...", 325, 26, 68);
			m_template.Button(IDC_S_MODEL_EDIT, L"Düzenle...", 325, 44, 68);
			m_template.Button(IDC_S_MODEL_COPY, L"Çoğalt", 325, 62, 68);
			m_template.Button(IDC_S_MODEL_DELETE, L"Sil", 325, 86, 68);
		}

		void Load() override
		{
			m_list.SubclassDlgItem(IDC_S_MODELS, this);
			InitListView(m_list, { { _T("Ad"), 30 }, { _T("Sağlayıcı"), 25 }, { _T("Model"), 25 }, { _T("Görsel"), 9 }, { _T("RAG"), 9 } });
			Refresh(0);
		}

	protected:
		DECLARE_MESSAGE_MAP()

		void Refresh(int select)
		{
			m_list.DeleteAllItems();
			for (int i = 0; i < (int)m_config.models.size(); i++)
			{
				const AiModelConfig &m = m_config.models[i];
				CString provider = AiModelConfig::ProviderName(m.provider);
				int paren = provider.Find(_T(" ("));
				if (paren > 0) provider = provider.Left(paren);
				m_list.InsertItem(i, m.name);
				m_list.SetItemText(i, 1, provider);
				m_list.SetItemText(i, 2, m.model);
				m_list.SetItemText(i, 3, m.supportsImages ? _T("evet") : _T("-"));
				m_list.SetItemText(i, 4, m.ragEnabled && !m.ragSources.empty() ? _T("açık") : _T("-"));
			}
			if (select >= 0 && select < m_list.GetItemCount())
			{
				m_list.SetItemState(select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
				m_list.EnsureVisible(select, FALSE);
			}
		}

		afx_msg void OnAdd()
		{
			AiModelConfig model;
			model.id = AiConfig::NewId();
			model.provider = AiProvider::OpenAICompatible;
			model.model.Empty();
			CAiModelDlg dlg(model, this);
			if (dlg.DoModal() == IDOK)
			{
				m_config.models.push_back(model);
				if (m_config.FindModel(m_config.defaultTextModelId) == nullptr) m_config.defaultTextModelId = model.id;
				if (model.supportsImages && m_config.FindModel(m_config.defaultImageModelId) == nullptr) m_config.defaultImageModelId = model.id;
				Refresh((int)m_config.models.size() - 1);
			}
		}

		afx_msg void OnEdit()
		{
			int sel = SelectedRow(m_list);
			if (sel < 0) return;
			AiModelConfig model = m_config.models[sel];
			CAiModelDlg dlg(model, this);
			if (dlg.DoModal() == IDOK)
			{
				m_config.models[sel] = model;
				Refresh(sel);
			}
		}

		afx_msg void OnCopy()
		{
			int sel = SelectedRow(m_list);
			if (sel < 0) return;
			AiModelConfig model = m_config.models[sel];
			model.id = AiConfig::NewId();
			model.name += _T(" (kopya)");
			m_config.models.push_back(model);
			Refresh((int)m_config.models.size() - 1);
		}

		afx_msg void OnDelete()
		{
			int sel = SelectedRow(m_list);
			if (sel < 0) return;
			CString msg;
			msg.Format(_T("'%s' modeli silinsin mi? Bu modeli kullanan eylemler varsayılan modele geçer."), (LPCTSTR)m_config.models[sel].name);
			if (MessageBox(msg, _T("Ditto AI"), MB_YESNO | MB_ICONQUESTION) != IDYES) return;
			CString id = m_config.models[sel].id;
			m_config.models.erase(m_config.models.begin() + sel);
			m_config.OnModelRemoved(id);
			Refresh(min(sel, (int)m_config.models.size() - 1));
		}

		afx_msg void OnDblClk(NMHDR *, LRESULT *pResult)
		{
			OnEdit();
			*pResult = 0;
		}

		AiConfig &m_config;
		CListCtrl m_list;
	};

	BEGIN_MESSAGE_MAP(CModelsPage, CAiPage)
		ON_BN_CLICKED(IDC_S_MODEL_ADD, OnAdd)
		ON_BN_CLICKED(IDC_S_MODEL_EDIT, OnEdit)
		ON_BN_CLICKED(IDC_S_MODEL_COPY, OnCopy)
		ON_BN_CLICKED(IDC_S_MODEL_DELETE, OnDelete)
		ON_NOTIFY(NM_DBLCLK, IDC_S_MODELS, OnDblClk)
	END_MESSAGE_MAP()

	// ---------------------------------------------------------------- actions & routing

	class CActionsPage : public CAiPage
	{
	public:
		CActionsPage(AiConfig &config) : m_config(config)
		{
			m_template.BeginChild(400, 250);
			m_template.Label(L"Varsayılan metin modeli:", 7, 8, 100);
			m_template.Combo(IDC_S_DEF_TEXT, 110, 6, 209, 120);
			m_template.Label(L"Varsayılan görsel modeli:", 7, 26, 100);
			m_template.Combo(IDC_S_DEF_IMAGE, 110, 24, 209, 120);
			m_template.Label(L"Çift Ctrl+C menüsündeki eylemler. Eylemde model seçilmemişse kopyalanan içeriğin türüne göre yukarıdaki varsayılan model kullanılır.", 7, 44, 386, 16);
			m_template.ListView(IDC_S_ACTIONS, 7, 62, 312, 178);
			m_template.Button(IDC_S_ACTION_ADD, L"Ekle...", 325, 62, 68);
			m_template.Button(IDC_S_ACTION_EDIT, L"Düzenle...", 325, 80, 68);
			m_template.Button(IDC_S_ACTION_COPY, L"Çoğalt", 325, 98, 68);
			m_template.Button(IDC_S_ACTION_DELETE, L"Sil", 325, 122, 68);
			m_template.Button(IDC_S_ACTION_UP, L"Yukarı", 325, 146, 68);
			m_template.Button(IDC_S_ACTION_DOWN, L"Aşağı", 325, 164, 68);
		}

		void Load() override
		{
			m_list.SubclassDlgItem(IDC_S_ACTIONS, this);
			InitListView(m_list, { { _T("Eylem"), 45 }, { _T("İçerik"), 20 }, { _T("Model"), 33 } });
			RefreshCombos();
			Refresh(0);
		}

		void OnPageShown() override
		{
			// models may have been added/removed on the other tab
			StoreCombos();
			RefreshCombos();
			Refresh(SelectedRow(m_list));
		}

		bool Save() override
		{
			StoreCombos();
			return true;
		}

	protected:
		DECLARE_MESSAGE_MAP()

		void StoreCombos()
		{
			CComboBox *pText = (CComboBox *)GetDlgItem(IDC_S_DEF_TEXT);
			CComboBox *pImage = (CComboBox *)GetDlgItem(IDC_S_DEF_IMAGE);
			if (pText->GetCount() > 0) m_config.defaultTextModelId = SelectedModelId(pText, m_config);
			if (pImage->GetCount() > 0) m_config.defaultImageModelId = SelectedModelId(pImage, m_config);
		}

		void RefreshCombos()
		{
			FillModelCombo((CComboBox *)GetDlgItem(IDC_S_DEF_TEXT), m_config, m_config.defaultTextModelId, NULL, false);
			FillModelCombo((CComboBox *)GetDlgItem(IDC_S_DEF_IMAGE), m_config, m_config.defaultImageModelId, NULL, true);
		}

		void Refresh(int select)
		{
			m_list.DeleteAllItems();
			for (int i = 0; i < (int)m_config.actions.size(); i++)
			{
				const AiAction &a = m_config.actions[i];
				const AiModelConfig *model = m_config.FindModel(a.modelId);
				m_list.InsertItem(i, a.name);
				m_list.SetItemText(i, 1, ContentTypeName(a.appliesTo));
				m_list.SetItemText(i, 2, model ? model->name : CString(_T("(varsayılan)")));
			}
			if (select >= 0 && select < m_list.GetItemCount())
			{
				m_list.SetItemState(select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
				m_list.EnsureVisible(select, FALSE);
			}
		}

		afx_msg void OnAdd()
		{
			AiAction action;
			action.id = AiConfig::NewId();
			CAiActionDlg dlg(action, m_config, this);
			if (dlg.DoModal() == IDOK)
			{
				m_config.actions.push_back(action);
				Refresh((int)m_config.actions.size() - 1);
			}
		}

		afx_msg void OnEdit()
		{
			int sel = SelectedRow(m_list);
			if (sel < 0) return;
			AiAction action = m_config.actions[sel];
			CAiActionDlg dlg(action, m_config, this);
			if (dlg.DoModal() == IDOK)
			{
				m_config.actions[sel] = action;
				Refresh(sel);
			}
		}

		// The copy goes right below the original, the list order is the menu order.
		afx_msg void OnCopy()
		{
			int sel = SelectedRow(m_list);
			if (sel < 0) return;
			AiAction action = m_config.actions[sel];
			action.id = AiConfig::NewId();
			action.name += _T(" (kopya)");
			m_config.actions.insert(m_config.actions.begin() + sel + 1, action);
			Refresh(sel + 1);
		}

		afx_msg void OnDelete()
		{
			int sel = SelectedRow(m_list);
			if (sel < 0) return;
			m_config.actions.erase(m_config.actions.begin() + sel);
			Refresh(min(sel, (int)m_config.actions.size() - 1));
		}

		void Move(int delta)
		{
			int sel = SelectedRow(m_list);
			int target = sel + delta;
			if (sel < 0 || target < 0 || target >= (int)m_config.actions.size()) return;
			std::swap(m_config.actions[sel], m_config.actions[target]);
			Refresh(target);
		}

		afx_msg void OnUp() { Move(-1); }
		afx_msg void OnDown() { Move(1); }

		afx_msg void OnDblClk(NMHDR *, LRESULT *pResult)
		{
			OnEdit();
			*pResult = 0;
		}

		AiConfig &m_config;
		CListCtrl m_list;
	};

	BEGIN_MESSAGE_MAP(CActionsPage, CAiPage)
		ON_BN_CLICKED(IDC_S_ACTION_ADD, OnAdd)
		ON_BN_CLICKED(IDC_S_ACTION_EDIT, OnEdit)
		ON_BN_CLICKED(IDC_S_ACTION_COPY, OnCopy)
		ON_BN_CLICKED(IDC_S_ACTION_DELETE, OnDelete)
		ON_BN_CLICKED(IDC_S_ACTION_UP, OnUp)
		ON_BN_CLICKED(IDC_S_ACTION_DOWN, OnDown)
		ON_NOTIFY(NM_DBLCLK, IDC_S_ACTIONS, OnDblClk)
	END_MESSAGE_MAP()

	// ---------------------------------------------------------------- general & notifications

	class CGeneralPage : public CAiPage
	{
	public:
		CGeneralPage(AiConfig &config, std::function<void(const AiNotifySettings &)> preview)
			: m_config(config), m_preview(preview)
		{
			m_template.BeginChild(400, 250);
			m_template.Check(IDC_G_ENABLED, L"Ctrl+C'ye iki kez basınca yapay zeka menüsünü aç", 7, 6, 380);
			m_template.Label(L"Çift basış aralığı (ms):", 7, 24, 110);
			m_template.Edit(IDC_G_INTERVAL, 120, 22, 40, 12, ES_NUMBER);

			m_template.Label(L"Bildirimler", 7, 46, 380);
			m_template.Label(L"Bu bildirimleri Ditto kendisi gösterir; Windows bildirim ayarları, odak modu ve 'Rahatsız etmeyin' bunları susturmaz.", 7, 56, 386, 16);
			m_template.Check(IDC_G_PROGRESS, L"İstek sürerken bildirim göster", 14, 76, 300);
			m_template.Check(IDC_G_SUCCESS, L"Cevap hazır olunca bildirim göster", 14, 90, 300);
			m_template.Check(IDC_G_ERRORS, L"Hataları bildir", 14, 104, 300);
			m_template.Check(IDC_G_PREVIEW, L"Bildirimde cevabın önizlemesini göster", 14, 118, 300);
			m_template.Check(IDC_G_SOUND, L"Sesli uyarı çal", 14, 132, 300);
			m_template.Label(L"Konum:", 14, 152, 100);
			m_template.Combo(IDC_G_POSITION, 120, 150, 90, 80);
			m_template.Label(L"Görünme süresi (sn):", 14, 170, 100);
			m_template.Edit(IDC_G_DURATION, 120, 168, 40, 12, ES_NUMBER);
			m_template.Button(IDC_G_SAMPLE, L"Örnek bildirim göster", 14, 188, 100, 14);
			m_template.Label(L"", 7, 220, 386, 24, IDC_G_PATH);
		}

		void Load() override
		{
			CheckDlgButton(IDC_G_ENABLED, m_config.enabled ? BST_CHECKED : BST_UNCHECKED);
			SetDlgItemInt(IDC_G_INTERVAL, m_config.doubleCopyMs, FALSE);
			const AiNotifySettings &n = m_config.notify;
			CheckDlgButton(IDC_G_PROGRESS, n.showProgress ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(IDC_G_SUCCESS, n.showSuccess ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(IDC_G_ERRORS, n.showErrors ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(IDC_G_PREVIEW, n.showPreview ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(IDC_G_SOUND, n.sound ? BST_CHECKED : BST_UNCHECKED);
			CComboBox *pPos = (CComboBox *)GetDlgItem(IDC_G_POSITION);
			for (LPCTSTR name : POSITION_NAMES)
			{
				pPos->AddString(name);
			}
			pPos->SetCurSel(n.position);
			SetDlgItemInt(IDC_G_DURATION, n.durationSec, FALSE);
			SetDlgItemText(IDC_G_PATH, _T("Ayar dosyası: ") + AiConfig::FilePath());
		}

		bool Save() override
		{
			int interval = 0, duration = 0;
			if (!ParseInt(this, IDC_G_INTERVAL, 150, 2000, interval, _T("Çift basış aralığı")) ||
				!ParseInt(this, IDC_G_DURATION, 1, 120, duration, _T("Görünme süresi")))
			{
				return false;
			}
			m_config.enabled = IsDlgButtonChecked(IDC_G_ENABLED) == BST_CHECKED;
			m_config.doubleCopyMs = interval;
			Collect(m_config.notify);
			m_config.notify.durationSec = duration;
			return true;
		}

	protected:
		DECLARE_MESSAGE_MAP()

		void Collect(AiNotifySettings &n)
		{
			n.showProgress = IsDlgButtonChecked(IDC_G_PROGRESS) == BST_CHECKED;
			n.showSuccess = IsDlgButtonChecked(IDC_G_SUCCESS) == BST_CHECKED;
			n.showErrors = IsDlgButtonChecked(IDC_G_ERRORS) == BST_CHECKED;
			n.showPreview = IsDlgButtonChecked(IDC_G_PREVIEW) == BST_CHECKED;
			n.sound = IsDlgButtonChecked(IDC_G_SOUND) == BST_CHECKED;
			n.position = max(0, ((CComboBox *)GetDlgItem(IDC_G_POSITION))->GetCurSel());
			n.durationSec = min(120, max(1, (int)GetDlgItemInt(IDC_G_DURATION, NULL, FALSE)));
		}

		afx_msg void OnSample()
		{
			AiNotifySettings n;
			Collect(n);
			if (m_preview)
			{
				m_preview(n);
			}
		}

		AiConfig &m_config;
		std::function<void(const AiNotifySettings &)> m_preview;
	};

	BEGIN_MESSAGE_MAP(CGeneralPage, CAiPage)
		ON_BN_CLICKED(IDC_G_SAMPLE, OnSample)
	END_MESSAGE_MAP()

	// ---------------------------------------------------------------- token usage

	LPCTSTR PERIOD_NAMES[] = { _T("Bugün"), _T("Son 7 gün"), _T("Bu ay"), _T("Tüm zamanlar") };

	// 1234567 -> "1.234.567"
	CString FormatCount(long long value)
	{
		CString digits;
		digits.Format(_T("%lld"), value);
		CString out;
		int length = digits.GetLength();
		for (int i = 0; i < length; i++)
		{
			if (i > 0 && (length - i) % 3 == 0)
			{
				out += _T('.');
			}
			out += digits[i];
		}
		return out;
	}

	class CUsagePage : public CAiPage
	{
	public:
		CUsagePage()
		{
			m_template.BeginChild(400, 250);
			m_template.Label(L"Dönem:", 7, 8, 40);
			m_template.Combo(IDC_U_PERIOD, 50, 6, 100, 80);
			m_template.ListView(IDC_U_LIST, 7, 26, 386, 160);
			m_template.Label(L"", 7, 192, 386, 18, IDC_U_TOTAL);
			m_template.Label(L"Sayılar, sağlayıcının her cevapta bildirdiği token'lardır. Girdi: istem, bağlam ve kopyalanan içerik. "
				L"Çıktı: cevap ve modelin düşünme token'ları. Bağlantı testleri sayılmaz.", 7, 214, 312, 26);
			m_template.Button(IDC_U_RESET, L"Sıfırla...", 325, 226, 68);
		}

		void Load() override
		{
			CComboBox *pPeriod = (CComboBox *)GetDlgItem(IDC_U_PERIOD);
			for (LPCTSTR name : PERIOD_NAMES)
			{
				pPeriod->AddString(name);
			}
			pPeriod->SetCurSel(2);	// this month

			m_list.SubclassDlgItem(IDC_U_LIST, this);
			m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
			CRect rc;
			m_list.GetClientRect(rc);
			struct { LPCTSTR title; int percent; int format; } columns[] =
			{
				{ _T("Model"), 34, LVCFMT_LEFT }, { _T("İstek"), 12, LVCFMT_RIGHT }, { _T("Girdi token"), 18, LVCFMT_RIGHT },
				{ _T("Çıktı token"), 18, LVCFMT_RIGHT }, { _T("Toplam"), 17, LVCFMT_RIGHT },
			};
			for (int i = 0; i < _countof(columns); i++)
			{
				m_list.InsertColumn(i, columns[i].title, columns[i].format, rc.Width() * columns[i].percent / 100);
			}
			Refresh();
		}

		void OnPageShown() override
		{
			Refresh();	// a request may have finished while the window was open
		}

	protected:
		DECLARE_MESSAGE_MAP()

		struct Row
		{
			CString modelId;
			CString name;
			CString lastDate;
			int requests = 0;
			int unknownRequests = 0;
			long long input = 0;
			long long output = 0;
		};

		void Refresh()
		{
			CString from;	// empty = all time
			switch (((CComboBox *)GetDlgItem(IDC_U_PERIOD))->GetCurSel())
			{
			case 0: from = AiUsageDate(); break;
			case 1: from = AiUsageDate(6); break;
			case 2: from = AiUsageDate().Left(8) + _T("01"); break;
			}

			// one row per model; the name is taken from the newest record
			std::vector<Row> rows;
			for (const AiUsageEntry &e : AiUsageLoad())
			{
				if (from.IsEmpty() == FALSE && e.date < from)
				{
					continue;
				}
				Row *row = nullptr;
				for (Row &r : rows)
				{
					if (r.modelId == e.modelId)
					{
						row = &r;
						break;
					}
				}
				if (row == nullptr)
				{
					rows.push_back(Row());
					row = &rows.back();
					row->modelId = e.modelId;
				}
				if (e.date >= row->lastDate)
				{
					row->lastDate = e.date;
					row->name = e.modelName.IsEmpty() ? e.model : e.modelName;
				}
				row->requests += e.requests;
				row->unknownRequests += e.unknownRequests;
				row->input += e.inputTokens;
				row->output += e.outputTokens;
			}
			std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.input + a.output > b.input + b.output; });

			Row total;
			m_list.DeleteAllItems();
			for (int i = 0; i < (int)rows.size(); i++)
			{
				const Row &r = rows[i];
				m_list.InsertItem(i, r.name);
				m_list.SetItemText(i, 1, FormatCount(r.requests));
				m_list.SetItemText(i, 2, FormatCount(r.input));
				m_list.SetItemText(i, 3, FormatCount(r.output));
				m_list.SetItemText(i, 4, FormatCount(r.input + r.output));
				total.requests += r.requests;
				total.unknownRequests += r.unknownRequests;
				total.input += r.input;
				total.output += r.output;
			}

			CString text;
			if (rows.empty())
			{
				text = _T("Bu dönemde kayıt yok.");
			}
			else
			{
				text.Format(_T("Toplam: %s istek · %s girdi + %s çıktı = %s token"), (LPCTSTR)FormatCount(total.requests),
					(LPCTSTR)FormatCount(total.input), (LPCTSTR)FormatCount(total.output), (LPCTSTR)FormatCount(total.input + total.output));
				if (total.unknownRequests > 0)
				{
					text.AppendFormat(_T("\n%d istekte sağlayıcı token sayısını bildirmedi, bu istekler toplama eklenemedi."), total.unknownRequests);
				}
			}
			SetDlgItemText(IDC_U_TOTAL, text);
		}

		afx_msg void OnPeriodChanged()
		{
			Refresh();
		}

		afx_msg void OnReset()
		{
			if (MessageBox(_T("Tüm token kullanım kayıtları silinsin mi?\n\nBu işlem hemen uygulanır ve geri alınamaz; 'İptal' düğmesi kayıtları geri getirmez."),
				_T("Ditto AI"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
			{
				return;
			}
			CString error;
			if (!AiUsageReset(&error))
			{
				MessageBox(error, _T("Ditto AI"), MB_ICONERROR);
			}
			Refresh();
		}

		CListCtrl m_list;
	};

	BEGIN_MESSAGE_MAP(CUsagePage, CAiPage)
		ON_CBN_SELCHANGE(IDC_U_PERIOD, OnPeriodChanged)
		ON_BN_CLICKED(IDC_U_RESET, OnReset)
	END_MESSAGE_MAP()
}

CAiSettingsDlg::CAiSettingsDlg(AiConfig &config, std::function<void(const AiNotifySettings &)> previewToast, CWnd *pParent, int startPage)
	: CAiTabbedDialog(L"Ditto - Yapay zeka ayarları", 420, 310, pParent)
{
	AddPage(_T("Modeller"), new CModelsPage(config));
	AddPage(_T("Eylemler ve yönlendirme"), new CActionsPage(config));
	AddPage(_T("Genel ve bildirimler"), new CGeneralPage(config, previewToast));
	AddPage(_T("Token kullanımı"), new CUsagePage());
	m_startPage = startPage;
}

CAiSettingsDlg::~CAiSettingsDlg()
{
	for (auto &page : m_pages)
	{
		delete page.second;
	}
}

// ===========================================================================
// Request details

namespace
{
	enum
	{
		IDC_D_TITLE = 5001, IDC_D_MODEL, IDC_D_STATE, IDC_D_TIME, IDC_D_TOKENS, IDC_D_INSTRUCTION,
		IDC_D_INPUT_LABEL, IDC_D_INPUT, IDC_D_RESULT_LABEL, IDC_D_RESULT, IDC_D_COPY,
	};

	void ValueLabel(CAiDialogTemplate &t, WORD id, short x, short y, short cx, short cy, bool singleLine)
	{
		t.Control((WORD)0x0082, L"", id, SS_LEFT | SS_NOPREFIX | (singleLine ? SS_ENDELLIPSIS : 0), 0, x, y, cx, cy);
	}

	void ReadOnlyText(CAiDialogTemplate &t, WORD id, short x, short y, short cx, short cy)
	{
		t.Control((WORD)0x0081, L"", id, WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, WS_EX_CLIENTEDGE, x, y, cx, cy);
	}

	CString FormatSeconds(ULONGLONG ms)
	{
		CString text;
		text.Format(_T("%.1f sn"), ms / 1000.0);
		text.Replace(_T('.'), _T(','));
		return text;
	}
}

BEGIN_MESSAGE_MAP(CAiDetailDlg, CDialog)
	ON_BN_CLICKED(IDC_D_COPY, OnCopyResult)
END_MESSAGE_MAP()

CAiDetailDlg::CAiDetailDlg()
{
	const short lx = 7, vx = 62, vw = 331, fullWidth = 386;
	m_template.Begin(L"Yapay zeka - istek ayrıntısı", 400, 322);
	m_template.Label(L"Eylem:", lx, 8, 52);
	ValueLabel(m_template, IDC_D_TITLE, vx, 8, vw, 8, true);
	m_template.Label(L"Model:", lx, 20, 52);
	ValueLabel(m_template, IDC_D_MODEL, vx, 20, vw, 8, true);
	m_template.Label(L"Durum:", lx, 32, 52);
	ValueLabel(m_template, IDC_D_STATE, vx, 32, vw, 16, false);
	m_template.Label(L"Zaman:", lx, 50, 52);
	ValueLabel(m_template, IDC_D_TIME, vx, 50, vw, 8, true);
	m_template.Label(L"Token:", lx, 62, 52);
	ValueLabel(m_template, IDC_D_TOKENS, vx, 62, vw, 8, true);
	m_template.Label(L"Talimat:", lx, 78, fullWidth);
	ReadOnlyText(m_template, IDC_D_INSTRUCTION, lx, 88, fullWidth, 28);
	m_template.Label(L"", lx, 122, fullWidth, 8, IDC_D_INPUT_LABEL);
	ReadOnlyText(m_template, IDC_D_INPUT, lx, 132, fullWidth, 50);
	m_template.Label(L"", lx, 188, fullWidth, 8, IDC_D_RESULT_LABEL);
	ReadOnlyText(m_template, IDC_D_RESULT, lx, 198, fullWidth, 98);
	m_template.Button(IDC_D_COPY, L"Cevabı kopyala", lx, 302, 80);
	m_template.Button(IDCANCEL, L"Kapat", 343, 302, 50, 14, true);
}

void CAiDetailDlg::Show(const AiRunInfo &info, CWnd *pOwner)
{
	m_info = info;
	if (m_hWnd == NULL)
	{
		if (!CreateIndirect(m_template.Get(), pOwner))
		{
			return;
		}
		ModifyStyleEx(0, WS_EX_APPWINDOW);	// taskbar button, so it cannot get lost behind other windows
	}
	else
	{
		Fill();
	}
	ShowWindow(SW_SHOWNORMAL);
	::SetForegroundWindow(m_hWnd);
}

void CAiDetailDlg::Update(const AiRunInfo &info)
{
	if (m_hWnd != NULL && m_info.id == info.id)
	{
		m_info = info;
		Fill();
	}
}

BOOL CAiDetailDlg::OnInitDialog()
{
	CDialog::OnInitDialog();
	for (int id : { IDC_D_INSTRUCTION, IDC_D_INPUT, IDC_D_RESULT })
	{
		SendDlgItemMessage(id, EM_SETLIMITTEXT, 0, 0);	// the default limit is 32K characters
	}
	Fill();
	GetDlgItem(IDCANCEL)->SetFocus();
	return FALSE;
}

void CAiDetailDlg::OnOK()
{
	DestroyWindow();
}

void CAiDetailDlg::OnCancel()
{
	DestroyWindow();
}

void CAiDetailDlg::Fill()
{
	const AiRunInfo &r = m_info;
	bool running = r.state == AiRunInfo::State::Running;

	SetDlgItemText(IDC_D_TITLE, r.title);
	SetDlgItemText(IDC_D_MODEL, r.modelDetail.IsEmpty() ? r.modelName : r.modelName + _T("  (") + r.modelDetail + _T(")"));

	CString state;
	switch (r.state)
	{
	case AiRunInfo::State::Running: state = _T("Çalışıyor..."); break;
	case AiRunInfo::State::Success: state = _T("Tamamlandı"); break;
	case AiRunInfo::State::Error: state = _T("Hata"); break;
	}
	if (r.clipboardStatus.IsEmpty() == FALSE)
	{
		state += _T(" · ") + r.clipboardStatus;
	}
	if (r.warning.IsEmpty() == FALSE)
	{
		state += _T("\n") + r.warning;
	}
	SetDlgItemText(IDC_D_STATE, state);

	CString time = r.started.Format(_T("%d.%m.%Y %H:%M:%S"));
	if (!running)
	{
		time += _T(" · ") + FormatSeconds(r.durationMs);
	}
	SetDlgItemText(IDC_D_TIME, time);

	CString tokens;
	if (r.usageKnown)
	{
		tokens.Format(_T("Girdi %s · çıktı %s · toplam %s"), (LPCTSTR)FormatCount(r.inputTokens),
			(LPCTSTR)FormatCount(r.outputTokens), (LPCTSTR)FormatCount(r.inputTokens + r.outputTokens));
	}
	else
	{
		tokens = running ? _T("-") : _T("Sağlayıcı bildirmedi");
	}
	SetDlgItemText(IDC_D_TOKENS, tokens);

	SetDlgItemText(IDC_D_INSTRUCTION, ToCrLf(r.instruction));

	CString inputLabel;
	if (r.isImage)
	{
		inputLabel.Format(_T("Gönderilen içerik: görsel, %d × %d piksel"), r.imageWidth, r.imageHeight);
		SetDlgItemText(IDC_D_INPUT, _T("(Görsel burada gösterilmez.)"));
	}
	else
	{
		inputLabel.Format(_T("Gönderilen içerik: metin, %s karakter"), (LPCTSTR)FormatCount(r.inputText.GetLength()));
		SetDlgItemText(IDC_D_INPUT, ToCrLf(r.inputText));
	}
	SetDlgItemText(IDC_D_INPUT_LABEL, inputLabel);

	if (running)
	{
		SetDlgItemText(IDC_D_RESULT_LABEL, _T("Cevap:"));
		SetDlgItemText(IDC_D_RESULT, _T("(Model çalışıyor, cevap gelince burada görünecek.)"));
	}
	else if (r.state == AiRunInfo::State::Error && r.result.IsEmpty())
	{
		SetDlgItemText(IDC_D_RESULT_LABEL, _T("Hata:"));
		SetDlgItemText(IDC_D_RESULT, ToCrLf(r.error));
	}
	else
	{
		SetDlgItemText(IDC_D_RESULT_LABEL, _T("Cevap:"));
		SetDlgItemText(IDC_D_RESULT, ToCrLf(r.result));
	}

	GetDlgItem(IDC_D_COPY)->EnableWindow(r.result.IsEmpty() == FALSE);
}

void CAiDetailDlg::OnCopyResult()
{
	CString text = ToCrLf(m_info.result);
	if (text.IsEmpty() || !OpenClipboard())
	{
		return;
	}
	::EmptyClipboard();
	size_t bytes = (text.GetLength() + 1) * sizeof(WCHAR);
	HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (hMem != NULL)
	{
		memcpy(::GlobalLock(hMem), (LPCWSTR)text, bytes);
		::GlobalUnlock(hMem);
		if (::SetClipboardData(CF_UNICODETEXT, hMem) == NULL)
		{
			::GlobalFree(hMem);
		}
	}
	::CloseClipboard();
}

// ===========================================================================
// Requests window

namespace
{
	enum { IDC_Q_LIST = 5101, IDC_Q_STATUS, IDC_Q_DETAILS };

	const UINT_PTR TIMER_RUNS_CLOCK = 1;

	CString StateName(AiRunInfo::State state)
	{
		switch (state)
		{
		case AiRunInfo::State::Running: return _T("Çalışıyor");
		case AiRunInfo::State::Success: return _T("Tamamlandı");
		default: return _T("Hata");
		}
	}

	CString RunDuration(const AiRunInfo &r)
	{
		return FormatSeconds(r.state == AiRunInfo::State::Running ? ::GetTickCount64() - r.startTick : r.durationMs);
	}

	CString RunContent(const AiRunInfo &r)
	{
		CString text;
		if (r.isImage)
		{
			text.Format(_T("Görsel %d × %d"), r.imageWidth, r.imageHeight);
			return text;
		}
		text = r.inputText.Left(300);
		text.Replace(_T('\r'), _T(' '));
		text.Replace(_T('\n'), _T(' '));
		text.Replace(_T('\t'), _T(' '));
		text.Trim();
		return text;
	}
}

BEGIN_MESSAGE_MAP(CAiRunsDlg, CDialog)
	ON_BN_CLICKED(IDC_Q_DETAILS, OnDetails)
	ON_NOTIFY(NM_DBLCLK, IDC_Q_LIST, OnListDblClick)
	ON_WM_TIMER()
END_MESSAGE_MAP()

CAiRunsDlg::CAiRunsDlg(std::function<void(int runId)> openDetail)
	: m_openDetail(openDetail)
{
	m_template.Begin(L"Yapay zeka - istekler", 460, 236);
	m_template.Label(L"Son 10 istek, en yenisi üstte. Çalışan istekler 10'dan fazla olsa da hepsi listelenir. "
		L"Ayrıntılar için çift tıklayın. Liste Ditto kapanınca silinir.", 7, 7, 446, 16);
	m_template.ListView(IDC_Q_LIST, 7, 26, 446, 182);
	m_template.Label(L"", 7, 217, 330, 8, IDC_Q_STATUS);
	m_template.Button(IDC_Q_DETAILS, L"Ayrıntılar", 346, 214, 50, 14, true);
	m_template.Button(IDCANCEL, L"Kapat", 403, 214, 50, 14);
}

void CAiRunsDlg::Show(const std::vector<AiRunInfo> &runs, CWnd *pOwner)
{
	m_runs = runs;
	if (m_hWnd == NULL)
	{
		if (!CreateIndirect(m_template.Get(), pOwner))
		{
			return;
		}
		ModifyStyleEx(0, WS_EX_APPWINDOW);	// taskbar button, so it cannot get lost behind other windows
	}
	else
	{
		Fill();
	}
	ShowWindow(SW_SHOWNORMAL);
	::SetForegroundWindow(m_hWnd);
}

void CAiRunsDlg::Update(const std::vector<AiRunInfo> &runs)
{
	if (m_hWnd != NULL)
	{
		m_runs = runs;
		Fill();
	}
}

BOOL CAiRunsDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	m_list.SubclassDlgItem(IDC_Q_LIST, this);
	m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	CRect rc;
	m_list.GetClientRect(rc);
	struct { LPCTSTR title; int percent; int format; } columns[] =
	{
		{ _T("Durum"), 12, LVCFMT_LEFT }, { _T("Başlangıç"), 11, LVCFMT_LEFT }, { _T("Süre"), 9, LVCFMT_RIGHT },
		{ _T("Eylem"), 22, LVCFMT_LEFT }, { _T("Model"), 18, LVCFMT_LEFT }, { _T("İçerik"), 27, LVCFMT_LEFT },
	};
	for (int i = 0; i < _countof(columns); i++)
	{
		m_list.InsertColumn(i, columns[i].title, columns[i].format, rc.Width() * columns[i].percent / 100);
	}

	Fill();
	SetTimer(TIMER_RUNS_CLOCK, 1000, NULL);	// running requests show their elapsed time
	m_list.SetFocus();
	return FALSE;
}

void CAiRunsDlg::OnOK()
{
	OnDetails();
}

void CAiRunsDlg::OnCancel()
{
	DestroyWindow();
}

void CAiRunsDlg::Fill()
{
	int selectedId = 0;
	int sel = m_list.GetNextItem(-1, LVNI_SELECTED);
	if (sel >= 0)
	{
		selectedId = (int)m_list.GetItemData(sel);
	}

	m_list.SetRedraw(FALSE);
	m_list.DeleteAllItems();
	int running = 0;
	sel = -1;
	for (int i = 0; i < (int)m_runs.size(); i++)
	{
		const AiRunInfo &r = m_runs[i];
		m_list.InsertItem(i, StateName(r.state));
		m_list.SetItemData(i, (DWORD_PTR)r.id);
		m_list.SetItemText(i, 1, r.started.Format(_T("%H:%M:%S")));
		m_list.SetItemText(i, 2, RunDuration(r));
		m_list.SetItemText(i, 3, r.title);
		m_list.SetItemText(i, 4, r.modelName);
		m_list.SetItemText(i, 5, RunContent(r));
		if (r.id == selectedId)
		{
			sel = i;
		}
		if (r.state == AiRunInfo::State::Running)
		{
			running++;
		}
	}
	if (sel < 0 && m_list.GetItemCount() > 0)
	{
		sel = 0;
	}
	if (sel >= 0)
	{
		m_list.SetItemState(sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
	}
	m_list.SetRedraw(TRUE);
	m_list.Invalidate();

	CString status;
	if (m_runs.empty())
	{
		status = _T("Henüz istek yok.");
	}
	else if (running == 0)
	{
		status = _T("Çalışan istek yok.");
	}
	else
	{
		status.Format(_T("%d istek çalışıyor."), running);
	}
	SetDlgItemText(IDC_Q_STATUS, status);
}

void CAiRunsDlg::UpdateDurations()
{
	for (int i = 0; i < m_list.GetItemCount() && i < (int)m_runs.size(); i++)
	{
		if (m_runs[i].state == AiRunInfo::State::Running)
		{
			m_list.SetItemText(i, 2, RunDuration(m_runs[i]));
		}
	}
}

void CAiRunsDlg::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == TIMER_RUNS_CLOCK)
	{
		UpdateDurations();
		return;
	}
	CDialog::OnTimer(nIDEvent);
}

void CAiRunsDlg::OnDetails()
{
	int sel = m_list.GetNextItem(-1, LVNI_SELECTED);
	if (sel >= 0 && m_openDetail)
	{
		m_openDetail((int)m_list.GetItemData(sel));
	}
}

void CAiRunsDlg::OnListDblClick(NMHDR *pNMHDR, LRESULT *pResult)
{
	const NMITEMACTIVATE *item = (const NMITEMACTIVATE *)pNMHDR;
	if (item->iItem >= 0 && m_openDetail)
	{
		m_openDetail((int)m_list.GetItemData(item->iItem));
	}
	*pResult = 0;
}
