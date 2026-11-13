#include "stdafx.h"
#include "AiToastWnd.h"

#include <algorithm>

namespace
{
	const UINT_PTR TIMER_AUTO_HIDE = 1;
	const UINT_PTR TIMER_FADE_OUT = 2;

	const int TOAST_WIDTH = 360;
	const int TOAST_MARGIN = 16;
	const int TOAST_GAP = 8;			// between stacked toasts
	const int TOAST_PADDING = 14;
	const int ACCENT_WIDTH = 4;
	const int MAX_BODY_HEIGHT = 140;
	const int FULL_ALPHA = 245;

	const COLORREF TOAST_COLOR_BACKGROUND = RGB(32, 33, 36);
	const COLORREF TOAST_COLOR_BORDER = RGB(60, 64, 67);
	const COLORREF TOAST_COLOR_TITLE = RGB(255, 255, 255);
	const COLORREF TOAST_COLOR_BODY = RGB(200, 202, 206);
	const COLORREF TOAST_COLOR_HINT = RGB(138, 180, 248);

	LPCTSTR CLICK_HINT = _T("Ayrıntılar için tıklayın");

	COLORREF AccentColor(AiToastKind kind)
	{
		switch (kind)
		{
		case AiToastKind::Success: return RGB(52, 168, 83);
		case AiToastKind::Error: return RGB(234, 67, 53);
		default: return RGB(66, 133, 244);
		}
	}
}

BEGIN_MESSAGE_MAP(CAiToastWnd, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_TIMER()
	ON_WM_LBUTTONUP()
	ON_WM_MOUSEACTIVATE()
END_MESSAGE_MAP()

CAiToastWnd::CAiToastWnd(CAiToastStack *stack)
	: m_stack(stack), m_key(0), m_shown(false), m_order(0), m_monitor(NULL), m_position(0),
	m_kind(AiToastKind::Info), m_clickHint(false), m_dpi(96), m_fontDpi(0), m_alpha(FULL_ALPHA)
{
}

CAiToastWnd::~CAiToastWnd()
{
	if (m_hWnd != NULL)
	{
		DestroyWindow();
	}
}

bool CAiToastWnd::EnsureCreated()
{
	if (m_hWnd != NULL)
	{
		return true;
	}

	CString className = AfxRegisterWndClass(CS_DROPSHADOW, ::LoadCursor(NULL, IDC_HAND), NULL, NULL);
	BOOL created = CreateEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
		className, _T("Ditto AI"), WS_POPUP, CRect(0, 0, 10, 10), NULL, 0);

	return created != FALSE;
}

void CAiToastWnd::CreateFonts(int dpi)
{
	if (m_fontDpi == dpi)
	{
		return;
	}

	m_titleFont.DeleteObject();
	m_bodyFont.DeleteObject();

	LOGFONT lf = { 0 };
	_tcscpy_s(lf.lfFaceName, _T("Segoe UI"));
	lf.lfQuality = CLEARTYPE_QUALITY;

	lf.lfHeight = -MulDiv(10, dpi, 72);
	lf.lfWeight = FW_SEMIBOLD;
	m_titleFont.CreateFontIndirect(&lf);

	lf.lfHeight = -MulDiv(9, dpi, 72);
	lf.lfWeight = FW_NORMAL;
	m_bodyFont.CreateFontIndirect(&lf);

	m_fontDpi = dpi;
}

CSize CAiToastWnd::CalcLayout(CDC &dc, int width, CRect &titleRect, CRect &bodyRect, CRect &hintRect)
{
	int left = Scale(ACCENT_WIDTH + TOAST_PADDING);
	int right = width - Scale(TOAST_PADDING);
	int top = Scale(TOAST_PADDING - 2);

	CFont *pOld = dc.SelectObject(&m_titleFont);
	titleRect.SetRect(left, top, right, top);
	dc.DrawText(m_title, titleRect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
	titleRect.right = right;

	int height = titleRect.bottom;

	bodyRect.SetRectEmpty();
	if (m_body.IsEmpty() == FALSE)
	{
		dc.SelectObject(&m_bodyFont);
		int bodyTop = titleRect.bottom + Scale(4);
		bodyRect.SetRect(left, bodyTop, right, bodyTop);
		dc.DrawText(m_body, bodyRect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
		bodyRect.right = right;
		if (bodyRect.Height() > Scale(MAX_BODY_HEIGHT))
		{
			bodyRect.bottom = bodyRect.top + Scale(MAX_BODY_HEIGHT);
		}
		height = bodyRect.bottom;
	}

	hintRect.SetRectEmpty();
	if (m_clickHint)
	{
		dc.SelectObject(&m_bodyFont);
		int hintTop = height + Scale(6);
		hintRect.SetRect(left, hintTop, right, hintTop);
		dc.DrawText(CLICK_HINT, hintRect, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
		hintRect.right = right;
		height = hintRect.bottom;
	}

	dc.SelectObject(pOld);

	return CSize(width, height + Scale(TOAST_PADDING));
}

bool CAiToastWnd::SetContent(const CString &title, const CString &body, AiToastKind kind, UINT timeoutMs, int position, bool clickHint)
{
	if (EnsureCreated() == false)
	{
		return false;
	}

	KillTimer(TIMER_AUTO_HIDE);
	KillTimer(TIMER_FADE_OUT);

	m_title = title;
	m_body = body;
	m_kind = kind;
	m_clickHint = clickHint;
	m_position = position;

	if (!m_shown)
	{
		// Show on the monitor the user is working on (the one under the mouse).
		// A toast that is already visible is updated where it is.
		CPoint cursor;
		::GetCursorPos(&cursor);
		m_monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
		MONITORINFO mi = { sizeof(MONITORINFO) };
		::GetMonitorInfo(m_monitor, &mi);
		m_work = mi.rcWork;

		// Move onto the target monitor first so the window DPI matches that monitor.
		SetWindowPos(NULL, m_work.left, m_work.top, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
		m_dpi = ::GetDpiForWindow(m_hWnd);
		if (m_dpi <= 0)
		{
			m_dpi = 96;
		}
		CreateFonts(m_dpi);
		m_shown = true;
	}

	CClientDC dc(this);
	m_size = CalcLayout(dc, Scale(TOAST_WIDTH), m_titleRect, m_bodyRect, m_hintRect);

	m_alpha = FULL_ALPHA;
	SetLayeredWindowAttributes(0, (BYTE)m_alpha, LWA_ALPHA);
	Invalidate();

	if (timeoutMs > 0)
	{
		SetTimer(TIMER_AUTO_HIDE, timeoutMs, NULL);
	}
	return true;
}

void CAiToastWnd::PlaceAt(int x, int y)
{
	SetWindowPos(&wndTopMost, x, y, m_size.cx, m_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	UpdateWindow();
}

void CAiToastWnd::HideToast(bool fade)
{
	if (m_hWnd == NULL || !m_shown)
	{
		return;
	}

	KillTimer(TIMER_AUTO_HIDE);

	if (fade && IsWindowVisible())
	{
		SetTimer(TIMER_FADE_OUT, 15, NULL);
	}
	else
	{
		HideNow(true);
	}
}

void CAiToastWnd::HideNow(bool notifyStack)
{
	KillTimer(TIMER_AUTO_HIDE);
	KillTimer(TIMER_FADE_OUT);
	ShowWindow(SW_HIDE);
	m_shown = false;
	m_alpha = FULL_ALPHA;
	SetLayeredWindowAttributes(0, (BYTE)m_alpha, LWA_ALPHA);

	if (notifyStack && m_stack != nullptr)
	{
		m_stack->OnToastHidden();	// the toasts above move into the free place
	}
}

void CAiToastWnd::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == TIMER_AUTO_HIDE)
	{
		HideToast(true);
		return;
	}

	if (nIDEvent == TIMER_FADE_OUT)
	{
		m_alpha -= 20;
		if (m_alpha <= 0)
		{
			HideNow(true);
		}
		else
		{
			SetLayeredWindowAttributes(0, (BYTE)m_alpha, LWA_ALPHA);
		}
		return;
	}

	CWnd::OnTimer(nIDEvent);
}

void CAiToastWnd::OnLButtonUp(UINT nFlags, CPoint point)
{
	int key = m_key;
	bool clickHint = m_clickHint;
	HideNow(true);
	if (m_stack != nullptr)
	{
		m_stack->OnToastClicked(key, clickHint);
	}
}

int CAiToastWnd::OnMouseActivate(CWnd *pDesktopWnd, UINT nHitTest, UINT message)
{
	return MA_NOACTIVATE;
}

BOOL CAiToastWnd::OnEraseBkgnd(CDC *pDC)
{
	return TRUE;
}

void CAiToastWnd::OnPaint()
{
	CPaintDC paintDC(this);

	CRect client;
	GetClientRect(client);

	// Double buffer to avoid flicker.
	CDC dc;
	dc.CreateCompatibleDC(&paintDC);
	CBitmap bitmap;
	bitmap.CreateCompatibleBitmap(&paintDC, client.Width(), client.Height());
	CBitmap *pOldBitmap = dc.SelectObject(&bitmap);

	dc.FillSolidRect(client, TOAST_COLOR_BACKGROUND);

	CRect accent(client.left, client.top, client.left + Scale(ACCENT_WIDTH), client.bottom);
	dc.FillSolidRect(accent, AccentColor(m_kind));

	CBrush border(TOAST_COLOR_BORDER);
	dc.FrameRect(client, &border);

	dc.SetBkMode(TRANSPARENT);

	CFont *pOldFont = dc.SelectObject(&m_titleFont);
	dc.SetTextColor(TOAST_COLOR_TITLE);
	CRect titleRect = m_titleRect;
	dc.DrawText(m_title, titleRect, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);

	if (m_body.IsEmpty() == FALSE)
	{
		dc.SelectObject(&m_bodyFont);
		dc.SetTextColor(TOAST_COLOR_BODY);
		CRect bodyRect = m_bodyRect;
		dc.DrawText(m_body, bodyRect, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_END_ELLIPSIS);
	}

	if (m_clickHint)
	{
		dc.SelectObject(&m_bodyFont);
		dc.SetTextColor(TOAST_COLOR_HINT);
		CRect hintRect = m_hintRect;
		dc.DrawText(CLICK_HINT, hintRect, DT_SINGLELINE | DT_NOPREFIX);
	}

	dc.SelectObject(pOldFont);

	paintDC.BitBlt(0, 0, client.Width(), client.Height(), &dc, 0, 0, SRCCOPY);
	dc.SelectObject(pOldBitmap);
}

// ===========================================================================
// CAiToastStack

void CAiToastStack::Show(int key, const CString &title, const CString &body, AiToastKind kind, UINT timeoutMs, int position, bool clickHint)
{
	// the visible toast with this key, else a hidden one to reuse, else a new one
	CAiToastWnd *toast = nullptr;
	for (auto &t : m_toasts)
	{
		if (t->IsShown() && t->Key() == key)
		{
			toast = t.get();
			break;
		}
	}
	if (toast == nullptr)
	{
		for (auto &t : m_toasts)
		{
			if (!t->IsShown())
			{
				toast = t.get();
				break;
			}
		}
	}
	if (toast == nullptr)
	{
		m_toasts.push_back(std::make_unique<CAiToastWnd>(this));
		toast = m_toasts.back().get();
	}

	if (!toast->IsShown())
	{
		toast->m_order = ++m_nextOrder;
	}
	toast->SetKey(key);
	if (toast->SetContent(title, body, kind, timeoutMs, position, clickHint))
	{
		Layout();
	}
}

void CAiToastStack::Hide(int key, bool fade)
{
	for (auto &t : m_toasts)
	{
		if (t->IsShown() && t->Key() == key)
		{
			t->HideToast(fade);
		}
	}
}

void CAiToastStack::DestroyAll()
{
	m_toasts.clear();
}

void CAiToastStack::OnToastClicked(int key, bool clickHint)
{
	if (m_clickTarget != NULL)
	{
		::PostMessage(m_clickTarget, WM_AI_TOAST_CLICKED, (WPARAM)key, clickHint ? 1 : 0);
	}
}

// Toasts on the same monitor and corner form a column: the oldest sits in the corner,
// newer ones line up away from it. Called whenever a toast appears, changes size or hides.
void CAiToastStack::Layout()
{
	if (m_inLayout)
	{
		return;
	}
	m_inLayout = true;

	std::vector<CAiToastWnd *> shown;
	for (auto &t : m_toasts)
	{
		if (t->IsShown())
		{
			shown.push_back(t.get());
		}
	}
	std::sort(shown.begin(), shown.end(), [](const CAiToastWnd *a, const CAiToastWnd *b) { return a->Order() < b->Order(); });

	while (!shown.empty())
	{
		CAiToastWnd *first = shown.front();
		std::vector<CAiToastWnd *> column;
		for (auto it = shown.begin(); it != shown.end();)
		{
			if ((*it)->Monitor() == first->Monitor() && (*it)->Position() == first->Position())
			{
				column.push_back(*it);
				it = shown.erase(it);
			}
			else
			{
				++it;
			}
		}

		CRect work = first->WorkArea();
		int position = first->Position();
		int margin = first->Scale(TOAST_MARGIN);
		int gap = first->Scale(TOAST_GAP);

		// more than the screen holds: the oldest give way to the newer ones
		for (;;)
		{
			int height = margin;
			for (CAiToastWnd *t : column)
			{
				height += t->Size().cy + gap;
			}
			if (column.size() <= 1 || height <= work.Height())
			{
				break;
			}
			column.front()->HideNow(false);
			column.erase(column.begin());
		}

		bool left = position == 2 || position == 3;
		bool top = position == 1 || position == 3;
		int offset = margin;
		for (CAiToastWnd *t : column)
		{
			CSize size = t->Size();
			int x = left ? work.left + margin : work.right - size.cx - margin;
			int y = top ? work.top + offset : work.bottom - offset - size.cy;
			t->PlaceAt(x, y);
			offset += size.cy + gap;
		}
	}

	m_inLayout = false;
}
