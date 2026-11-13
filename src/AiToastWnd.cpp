#include "stdafx.h"
#include "AiToastWnd.h"

namespace
{
	const UINT_PTR TIMER_AUTO_HIDE = 1;
	const UINT_PTR TIMER_FADE_OUT = 2;

	const int TOAST_WIDTH = 360;
	const int TOAST_MARGIN = 16;
	const int TOAST_PADDING = 14;
	const int ACCENT_WIDTH = 4;
	const int MAX_BODY_HEIGHT = 140;
	const int FULL_ALPHA = 245;

	const COLORREF TOAST_COLOR_BACKGROUND = RGB(32, 33, 36);
	const COLORREF TOAST_COLOR_BORDER = RGB(60, 64, 67);
	const COLORREF TOAST_COLOR_TITLE = RGB(255, 255, 255);
	const COLORREF TOAST_COLOR_BODY = RGB(200, 202, 206);

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

CAiToastWnd::CAiToastWnd()
	: m_kind(AiToastKind::Info), m_dpi(96), m_fontDpi(0), m_alpha(FULL_ALPHA)
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

CSize CAiToastWnd::CalcLayout(CDC &dc, int width, CRect &titleRect, CRect &bodyRect)
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

	dc.SelectObject(pOld);

	return CSize(width, height + Scale(TOAST_PADDING));
}

void CAiToastWnd::ShowToast(const CString &title, const CString &body, AiToastKind kind, UINT timeoutMs, int position)
{
	if (EnsureCreated() == false)
	{
		return;
	}

	KillTimer(TIMER_AUTO_HIDE);
	KillTimer(TIMER_FADE_OUT);

	m_title = title;
	m_body = body;
	m_kind = kind;

	// Show on the monitor the user is working on (the one under the mouse).
	CPoint cursor;
	::GetCursorPos(&cursor);
	HMONITOR hMonitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = { sizeof(MONITORINFO) };
	::GetMonitorInfo(hMonitor, &mi);
	CRect work(mi.rcWork);

	// Move onto the target monitor first so the window DPI matches that monitor.
	SetWindowPos(NULL, work.left, work.top, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
	m_dpi = ::GetDpiForWindow(m_hWnd);
	if (m_dpi <= 0)
	{
		m_dpi = 96;
	}
	CreateFonts(m_dpi);

	CClientDC dc(this);
	CSize size = CalcLayout(dc, Scale(TOAST_WIDTH), m_titleRect, m_bodyRect);

	bool left = position == 2 || position == 3;
	bool top = position == 1 || position == 3;
	int x = left ? work.left + Scale(TOAST_MARGIN) : work.right - size.cx - Scale(TOAST_MARGIN);
	int y = top ? work.top + Scale(TOAST_MARGIN) : work.bottom - size.cy - Scale(TOAST_MARGIN);

	m_alpha = FULL_ALPHA;
	SetLayeredWindowAttributes(0, (BYTE)m_alpha, LWA_ALPHA);
	SetWindowPos(&wndTopMost, x, y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	Invalidate();
	UpdateWindow();

	if (timeoutMs > 0)
	{
		SetTimer(TIMER_AUTO_HIDE, timeoutMs, NULL);
	}
}

void CAiToastWnd::HideToast(bool fade)
{
	if (m_hWnd == NULL)
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
		KillTimer(TIMER_FADE_OUT);
		ShowWindow(SW_HIDE);
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
			KillTimer(TIMER_FADE_OUT);
			ShowWindow(SW_HIDE);
			m_alpha = FULL_ALPHA;
			SetLayeredWindowAttributes(0, (BYTE)m_alpha, LWA_ALPHA);
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
	HideToast(false);
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

	dc.SelectObject(pOldFont);

	paintDC.BitBlt(0, 0, client.Width(), client.Height(), &dc, 0, 0, SRCCOPY);
	dc.SelectObject(pOldBitmap);
}
