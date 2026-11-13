#pragma once

#include <vector>
#include <functional>
#include "AiConfig.h"

// Settings UI of the AI module. Dialogs are built from in-memory templates so the
// module needs no changes to Ditto's resource script (which is code page 1252 and
// cannot hold Turkish text).

class CAiDialogTemplate
{
public:
	void Begin(LPCWSTR title, short cx, short cy);		// top level dialog
	void BeginChild(short cx, short cy);				// page inside a tab control

	void Control(LPCWSTR className, LPCWSTR text, WORD id, DWORD style, DWORD exStyle, short x, short y, short cx, short cy);
	void Control(WORD classAtom, LPCWSTR text, WORD id, DWORD style, DWORD exStyle, short x, short y, short cx, short cy);

	// helpers
	void Label(LPCWSTR text, short x, short y, short cx, short cy = 8, WORD id = 0xFFFF);
	void Edit(WORD id, short x, short y, short cx, short cy = 12, DWORD extraStyle = 0);
	void MultiLineEdit(WORD id, short x, short y, short cx, short cy, bool wantReturn = true);
	void Button(WORD id, LPCWSTR text, short x, short y, short cx = 50, short cy = 14, bool isDefault = false);
	void Check(WORD id, LPCWSTR text, short x, short y, short cx, short cy = 10);
	void Combo(WORD id, short x, short y, short cx, short dropHeight = 100);
	void ListBox(WORD id, short x, short y, short cx, short cy);
	void ListView(WORD id, short x, short y, short cx, short cy);

	LPCDLGTEMPLATE Get() const { return (LPCDLGTEMPLATE)m_data.data(); }

private:
	void Header(DWORD style, DWORD exStyle, LPCWSTR title, short cx, short cy);
	void AlignDword();
	void AddWord(WORD w) { m_data.push_back(w); }
	void AddDword(DWORD d) { m_data.push_back(LOWORD(d)); m_data.push_back(HIWORD(d)); }
	void AddString(LPCWSTR s);

	std::vector<WORD> m_data;
	size_t m_countIndex = 0;
};

// ---------------------------------------------------------------------------
// Tabs

class CAiPage : public CDialog
{
public:
	CAiPage() {}
	bool CreatePage(CWnd *pParent);

	virtual void Load() {}
	virtual bool Save() { return true; }		// false = invalid input, keep the dialog open
	virtual void OnPageShown() {}

protected:
	virtual BOOL OnInitDialog();
	virtual void OnOK() {}						// Enter must not close a page
	virtual void OnCancel() {}

	CAiDialogTemplate m_template;
};

class CAiTabbedDialog : public CDialog
{
public:
	CAiTabbedDialog(LPCWSTR title, short cx, short cy, CWnd *pParent);
	virtual ~CAiTabbedDialog() {}

	void AddPage(LPCTSTR title, CAiPage *pPage);
	void SelectPage(int index);

protected:
	virtual BOOL OnInitDialog();
	virtual void OnOK();

	DECLARE_MESSAGE_MAP()
	afx_msg void OnTabChanged(NMHDR *pNMHDR, LRESULT *pResult);

	CAiDialogTemplate m_template;
	CTabCtrl m_tab;
	std::vector<std::pair<CString, CAiPage *>> m_pages;
	int m_startPage = 0;
};

// ---------------------------------------------------------------------------
// Settings window: Models | Actions & routing | General & notifications | Token usage

class CAiSettingsDlg : public CAiTabbedDialog
{
public:
	CAiSettingsDlg(AiConfig &config, std::function<void(const AiNotifySettings &)> previewToast, CWnd *pParent, int startPage = 0);
	virtual ~CAiSettingsDlg();
};

// ---------------------------------------------------------------------------
// Model editor: Connection | Fine tuning | RAG

class CAiModelDlg : public CAiTabbedDialog
{
public:
	CAiModelDlg(AiModelConfig &model, CWnd *pParent);
	virtual ~CAiModelDlg();

private:
	AiModelConfig &m_model;
};

// ---------------------------------------------------------------------------
// Action editor

class CAiActionDlg : public CDialog
{
public:
	CAiActionDlg(AiAction &action, const AiConfig &config, CWnd *pParent);

protected:
	virtual BOOL OnInitDialog();
	virtual void OnOK();

	CAiDialogTemplate m_template;
	AiAction &m_action;
	const AiConfig &m_config;
};
