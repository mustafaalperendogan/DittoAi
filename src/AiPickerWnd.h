#pragma once

#include <vector>

// Popup shown after a double Ctrl+C: a search box over the list of actions.
// Typing filters the list; text that matches nothing can be sent as a free instruction.
// The result is posted to the owner as WM_AI_PICKED (lParam = AiPickResult*, owner deletes).

#define WM_AI_PICKED	(WM_USER + 303)

struct AiPickerItem
{
	int actionIndex = -1;
	CString title;
	CString detail;			// shown on the right (target model)
	CString searchText;		// extra text matched by the search (instruction)
};

struct AiPickResult
{
	enum Kind { Action, Custom, Settings } kind = Action;
	int actionIndex = -1;
	CString customInstruction;
};

class CAiPickerList : public CListBox
{
protected:
	DECLARE_MESSAGE_MAP()
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
};

class CAiPickerWnd : public CWnd
{
public:
	CAiPickerWnd();
	virtual ~CAiPickerWnd();

	void Show(CWnd *pOwner, const CString &header, const std::vector<AiPickerItem> &items, const CString &customDetail, CPoint pt);
	void Close();
	bool IsOpen() const { return m_hWnd != NULL && ::IsWindowVisible(m_hWnd); }

	void CommitSelection();

protected:
	virtual BOOL PreTranslateMessage(MSG *pMsg);

	DECLARE_MESSAGE_MAP()
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	afx_msg void OnActivate(UINT nState, CWnd *pWndOther, BOOL bMinimized);
	afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);
	afx_msg void OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT lpMeasureItemStruct);
	afx_msg void OnSearchChanged();
	afx_msg void OnListDblClk();
	afx_msg HBRUSH OnCtlColor(CDC *pDC, CWnd *pWnd, UINT nCtlColor);
	afx_msg LRESULT OnDeferredClose(WPARAM wParam, LPARAM lParam);

	struct Row
	{
		enum Type { ActionRow, CustomRow, SettingsRow } type;
		int itemIndex;
	};

	bool EnsureCreated();
	void CreateFonts();
	void Layout();
	void Refilter();
	void MoveSelection(int delta);
	void Finish(AiPickResult *result);
	int Scale(int v) const { return MulDiv(v, m_dpi, 96); }

	CWnd *m_pOwner;
	CString m_header;
	CString m_customDetail;
	std::vector<AiPickerItem> m_items;
	std::vector<Row> m_rows;

	CEdit m_search;
	CAiPickerList m_list;
	CFont m_font;
	CFont m_smallFont;
	CFont m_headerFont;
	CBrush m_editBrush;
	int m_dpi;
	int m_fontDpi;
	bool m_closing;
};
