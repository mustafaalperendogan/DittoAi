#include "stdafx.h"
#include "AiPickerWnd.h"

namespace
{
	const UINT IDC_PICKER_SEARCH = 100;
	const UINT IDC_PICKER_LIST = 101;
	const UINT WM_PICKER_DEFERRED_CLOSE = WM_USER + 1;

	const int WIDTH = 460;
	const int PADDING = 10;
	const int HEADER_HEIGHT = 26;
	const int SEARCH_HEIGHT = 30;
	const int ITEM_HEIGHT = 30;
	const int MAX_VISIBLE_ITEMS = 11;
	const int FOOTER_HEIGHT = 24;

	const COLORREF BACKGROUND = RGB(250, 250, 251);
	const COLORREF BORDER = RGB(200, 203, 208);
	const COLORREF HEADER_TEXT = RGB(95, 99, 104);
	const COLORREF ITEM_TEXT = RGB(32, 33, 36);
	const COLORREF DETAIL_TEXT = RGB(120, 124, 130);
	const COLORREF SELECTED_BACK = RGB(225, 236, 254);
	const COLORREF SELECTED_BAR = RGB(26, 115, 232);
	const COLORREF SPECIAL_TEXT = RGB(26, 115, 232);

	CString Fold(const CString &text)
	{
		CString out = text;
		for (int i = 0; i < out.GetLength(); i++)
		{
			TCHAR c = out[i];
			if (c == _T('I') || c == 0x0130 || c == 0x0131) out.SetAt(i, _T('i'));
		}
		out.MakeLower();
		return out;
	}

	// every search word must appear somewhere
	bool Matches(const CString &haystack, const CString &query)
	{
		int pos = 0;
		CString word = query.Tokenize(_T(" "), pos);
		while (pos != -1)
		{
			if (haystack.Find(word) < 0)
			{
				return false;
			}
			word = query.Tokenize(_T(" "), pos);
		}
		return true;
	}
}

// ---------------------------------------------------------------------------

BEGIN_MESSAGE_MAP(CAiPickerList, CListBox)
	ON_WM_LBUTTONUP()
END_MESSAGE_MAP()

void CAiPickerList::OnLButtonUp(UINT nFlags, CPoint point)
{
	CListBox::OnLButtonUp(nFlags, point);

	BOOL outside = TRUE;
	ItemFromPoint(point, outside);
	if (!outside)
	{
		((CAiPickerWnd *)GetParent())->CommitSelection();
	}
}

// ---------------------------------------------------------------------------

BEGIN_MESSAGE_MAP(CAiPickerWnd, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_ACTIVATE()
	ON_WM_DRAWITEM()
	ON_WM_MEASUREITEM()
	ON_WM_CTLCOLOR()
	ON_EN_CHANGE(IDC_PICKER_SEARCH, OnSearchChanged)
	ON_LBN_DBLCLK(IDC_PICKER_LIST, OnListDblClk)
	ON_MESSAGE(WM_PICKER_DEFERRED_CLOSE, OnDeferredClose)
END_MESSAGE_MAP()

CAiPickerWnd::CAiPickerWnd()
	: m_pOwner(NULL), m_dpi(96), m_fontDpi(0), m_closing(false)
{
}

CAiPickerWnd::~CAiPickerWnd()
{
	if (m_hWnd != NULL)
	{
		DestroyWindow();
	}
}

bool CAiPickerWnd::EnsureCreated()
{
	if (m_hWnd != NULL)
	{
		return true;
	}

	CString className = AfxRegisterWndClass(CS_DROPSHADOW, ::LoadCursor(NULL, IDC_ARROW), NULL, NULL);
	if (!CreateEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, className, _T("Ditto AI"), WS_POPUP | WS_CLIPCHILDREN, CRect(0, 0, 10, 10), NULL, 0))
	{
		return false;
	}

	m_search.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, CRect(0, 0, 10, 10), this, IDC_PICKER_SEARCH);
	m_list.Create(WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
		CRect(0, 0, 10, 10), this, IDC_PICKER_LIST);
	m_search.SetCueBanner(_T("Ara veya talimat yaz..."), TRUE);
	m_editBrush.CreateSolidBrush(RGB(255, 255, 255));
	return true;
}

void CAiPickerWnd::CreateFonts()
{
	if (m_fontDpi == m_dpi)
	{
		return;
	}
	m_font.DeleteObject();
	m_smallFont.DeleteObject();
	m_headerFont.DeleteObject();

	LOGFONT lf = { 0 };
	_tcscpy_s(lf.lfFaceName, _T("Segoe UI"));
	lf.lfQuality = CLEARTYPE_QUALITY;

	lf.lfHeight = -MulDiv(10, m_dpi, 72);
	m_font.CreateFontIndirect(&lf);

	lf.lfHeight = -MulDiv(9, m_dpi, 72);
	m_smallFont.CreateFontIndirect(&lf);

	lf.lfWeight = FW_SEMIBOLD;
	m_headerFont.CreateFontIndirect(&lf);

	m_fontDpi = m_dpi;

	m_search.SetFont(&m_font);
	m_list.SetFont(&m_font);
	m_list.SetItemHeight(0, Scale(ITEM_HEIGHT));
}

void CAiPickerWnd::Show(CWnd *pOwner, const CString &header, const std::vector<AiPickerItem> &items, const CString &customDetail, CPoint pt)
{
	if (!EnsureCreated())
	{
		return;
	}

	m_pOwner = pOwner;
	m_header = header;
	m_items = items;
	m_customDetail = customDetail;
	m_closing = false;

	// DPI of the monitor under the cursor
	HMONITOR hMonitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = { sizeof(MONITORINFO) };
	::GetMonitorInfo(hMonitor, &mi);
	CRect work(mi.rcWork);
	SetWindowPos(NULL, work.left, work.top, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
	m_dpi = ::GetDpiForWindow(m_hWnd);
	if (m_dpi <= 0) m_dpi = 96;
	CreateFonts();

	m_search.SetWindowText(_T(""));
	Refilter();

	int width = Scale(WIDTH);
	int listHeight = Scale(ITEM_HEIGHT) * MAX_VISIBLE_ITEMS;
	int height = Scale(PADDING) + Scale(HEADER_HEIGHT) + Scale(SEARCH_HEIGHT) + Scale(6) + listHeight + Scale(FOOTER_HEIGHT);

	int x = pt.x;
	int y = pt.y + Scale(8);
	if (x + width > work.right) x = work.right - width;
	if (x < work.left) x = work.left;
	if (y + height > work.bottom) y = max(work.top, pt.y - height - Scale(8));

	SetWindowPos(&wndTopMost, x, y, width, height, SWP_SHOWWINDOW);
	Layout();

	// take focus so typing goes to the search box
	HWND foreground = ::GetForegroundWindow();
	DWORD foregroundThread = foreground ? ::GetWindowThreadProcessId(foreground, NULL) : 0;
	DWORD currentThread = ::GetCurrentThreadId();
	bool attached = foregroundThread != 0 && foregroundThread != currentThread && ::AttachThreadInput(foregroundThread, currentThread, TRUE);
	::BringWindowToTop(m_hWnd);
	::SetForegroundWindow(m_hWnd);
	m_search.SetFocus();
	if (attached)
	{
		::AttachThreadInput(foregroundThread, currentThread, FALSE);
	}

	Invalidate();
}

void CAiPickerWnd::Layout()
{
	CRect client;
	GetClientRect(client);
	int pad = Scale(PADDING);
	int top = pad + Scale(HEADER_HEIGHT);

	m_search.MoveWindow(pad + Scale(8), top + Scale(6), client.Width() - 2 * pad - Scale(16), Scale(SEARCH_HEIGHT) - Scale(12));
	int listTop = top + Scale(SEARCH_HEIGHT) + Scale(6);
	m_list.MoveWindow(1, listTop, client.Width() - 2, client.bottom - Scale(FOOTER_HEIGHT) - listTop);
}

void CAiPickerWnd::Refilter()
{
	CString query;
	m_search.GetWindowText(query);
	query.Trim();
	CString folded = Fold(query);

	m_rows.clear();
	for (int i = 0; i < (int)m_items.size(); i++)
	{
		if (folded.IsEmpty() || Matches(Fold(m_items[i].title + _T(" ") + m_items[i].detail + _T(" ") + m_items[i].searchText), folded))
		{
			m_rows.push_back({ Row::ActionRow, i });
		}
	}
	if (query.IsEmpty() == FALSE)
	{
		m_rows.push_back({ Row::CustomRow, -1 });
	}
	m_rows.push_back({ Row::SettingsRow, -1 });

	m_list.SetRedraw(FALSE);
	m_list.ResetContent();
	for (size_t i = 0; i < m_rows.size(); i++)
	{
		m_list.AddString(_T(""));
	}
	m_list.SetCurSel(0);
	m_list.SetRedraw(TRUE);
	m_list.Invalidate();
}

void CAiPickerWnd::MoveSelection(int delta)
{
	int count = m_list.GetCount();
	if (count == 0)
	{
		return;
	}
	int sel = m_list.GetCurSel();
	sel = (sel < 0) ? 0 : sel + delta;
	sel = max(0, min(count - 1, sel));
	m_list.SetCurSel(sel);
}

BOOL CAiPickerWnd::PreTranslateMessage(MSG *pMsg)
{
	if (pMsg->message == WM_KEYDOWN)
	{
		switch (pMsg->wParam)
		{
		case VK_DOWN: MoveSelection(1); return TRUE;
		case VK_UP: MoveSelection(-1); return TRUE;
		case VK_NEXT: MoveSelection(MAX_VISIBLE_ITEMS - 1); return TRUE;
		case VK_PRIOR: MoveSelection(-(MAX_VISIBLE_ITEMS - 1)); return TRUE;
		case VK_RETURN: CommitSelection(); return TRUE;
		case VK_ESCAPE: Finish(NULL); return TRUE;
		case VK_TAB: return TRUE;
		}
	}
	return CWnd::PreTranslateMessage(pMsg);
}

void CAiPickerWnd::CommitSelection()
{
	int sel = m_list.GetCurSel();
	if (sel < 0 || sel >= (int)m_rows.size())
	{
		return;
	}

	AiPickResult *result = new AiPickResult();
	const Row &row = m_rows[sel];
	switch (row.type)
	{
	case Row::ActionRow:
		result->kind = AiPickResult::Action;
		result->actionIndex = m_items[row.itemIndex].actionIndex;
		break;
	case Row::CustomRow:
		result->kind = AiPickResult::Custom;
		m_search.GetWindowText(result->customInstruction);
		result->customInstruction.Trim();
		break;
	case Row::SettingsRow:
		result->kind = AiPickResult::Settings;
		break;
	}
	Finish(result);
}

void CAiPickerWnd::Finish(AiPickResult *result)
{
	if (m_closing)
	{
		delete result;
		return;
	}
	m_closing = true;
	ShowWindow(SW_HIDE);

	if (m_pOwner != NULL && ::IsWindow(m_pOwner->m_hWnd))
	{
		m_pOwner->PostMessage(WM_AI_PICKED, 0, (LPARAM)result);
	}
	else
	{
		delete result;
	}
}

void CAiPickerWnd::Close()
{
	if (m_hWnd != NULL && IsWindowVisible())
	{
		Finish(NULL);
	}
}

void CAiPickerWnd::OnActivate(UINT nState, CWnd *pWndOther, BOOL bMinimized)
{
	CWnd::OnActivate(nState, pWndOther, bMinimized);
	if (nState == WA_INACTIVE && IsWindowVisible())
	{
		// clicked elsewhere: close after the activation change has finished
		PostMessage(WM_PICKER_DEFERRED_CLOSE);
	}
}

LRESULT CAiPickerWnd::OnDeferredClose(WPARAM wParam, LPARAM lParam)
{
	if (IsWindowVisible() && ::GetForegroundWindow() != m_hWnd)
	{
		Finish(NULL);
	}
	return 0;
}

void CAiPickerWnd::OnSearchChanged()
{
	Refilter();
}

void CAiPickerWnd::OnListDblClk()
{
	CommitSelection();
}

HBRUSH CAiPickerWnd::OnCtlColor(CDC *pDC, CWnd *pWnd, UINT nCtlColor)
{
	if (pWnd->GetSafeHwnd() == m_search.GetSafeHwnd())
	{
		pDC->SetBkColor(RGB(255, 255, 255));
		pDC->SetTextColor(ITEM_TEXT);
		return m_editBrush;
	}
	return CWnd::OnCtlColor(pDC, pWnd, nCtlColor);
}

BOOL CAiPickerWnd::OnEraseBkgnd(CDC *pDC)
{
	return TRUE;
}

void CAiPickerWnd::OnPaint()
{
	CPaintDC dc(this);
	CRect client;
	GetClientRect(client);

	dc.FillSolidRect(client, BACKGROUND);
	CBrush border(BORDER);
	dc.FrameRect(client, &border);

	int pad = Scale(PADDING);
	dc.SetBkMode(TRANSPARENT);

	// header: what was copied
	CRect header(pad, pad, client.right - pad, pad + Scale(HEADER_HEIGHT));
	CFont *pOld = dc.SelectObject(&m_headerFont);
	dc.SetTextColor(HEADER_TEXT);
	dc.DrawText(m_header, header, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

	// search box frame
	CRect searchFrame(pad, header.bottom, client.right - pad, header.bottom + Scale(SEARCH_HEIGHT));
	dc.FillSolidRect(searchFrame, RGB(255, 255, 255));
	CBrush searchBorder(SELECTED_BAR);
	dc.FrameRect(searchFrame, &searchBorder);

	// footer hint
	CRect footer(pad, client.bottom - Scale(FOOTER_HEIGHT), client.right - pad, client.bottom);
	dc.SelectObject(&m_smallFont);
	dc.SetTextColor(DETAIL_TEXT);
	dc.DrawText(_T("↑↓ seç   Enter gönder   Esc kapat"), footer, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	dc.SelectObject(pOld);
}

void CAiPickerWnd::OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT lpMeasureItemStruct)
{
	lpMeasureItemStruct->itemHeight = Scale(ITEM_HEIGHT);
}

void CAiPickerWnd::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT dis)
{
	if (nIDCtl != IDC_PICKER_LIST || dis->itemID == (UINT)-1 || dis->itemID >= m_rows.size())
	{
		CWnd::OnDrawItem(nIDCtl, dis);
		return;
	}

	CDC *pDC = CDC::FromHandle(dis->hDC);
	CRect rc(dis->rcItem);
	bool selected = (dis->itemState & ODS_SELECTED) != 0;

	pDC->FillSolidRect(rc, selected ? SELECTED_BACK : BACKGROUND);
	if (selected)
	{
		pDC->FillSolidRect(rc.left, rc.top, Scale(3), rc.Height(), SELECTED_BAR);
	}
	pDC->SetBkMode(TRANSPARENT);

	const Row &row = m_rows[dis->itemID];
	CString title, detail;
	COLORREF titleColor = ITEM_TEXT;

	switch (row.type)
	{
	case Row::ActionRow:
		title = m_items[row.itemIndex].title;
		detail = m_items[row.itemIndex].detail;
		break;
	case Row::CustomRow:
	{
		CString query;
		m_search.GetWindowText(query);
		query.Trim();
		title = _T("Talimat olarak gönder: \"") + query + _T("\"");
		detail = m_customDetail;
		titleColor = SPECIAL_TEXT;
		break;
	}
	case Row::SettingsRow:
		title = _T("Ayarlar...");
		detail = _T("modeller, eylemler, bildirimler");
		titleColor = DETAIL_TEXT;
		break;
	}

	int pad = Scale(PADDING) + Scale(6);
	CRect textRect(rc.left + pad, rc.top, rc.right - Scale(8), rc.bottom);

	// detail on the right, at most 40% of the width
	CFont *pOld = pDC->SelectObject(&m_smallFont);
	CRect detailRect = textRect;
	if (detail.IsEmpty() == FALSE)
	{
		CSize size = pDC->GetTextExtent(detail);
		int maxWidth = textRect.Width() * 2 / 5;
		detailRect.left = textRect.right - min((int)size.cx, maxWidth);
		pDC->SetTextColor(DETAIL_TEXT);
		pDC->DrawText(detail, detailRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		textRect.right = detailRect.left - Scale(12);
	}

	pDC->SelectObject(&m_font);
	pDC->SetTextColor(titleColor);
	pDC->DrawText(title, textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
	pDC->SelectObject(pOld);
}
