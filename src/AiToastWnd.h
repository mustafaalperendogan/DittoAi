#pragma once

// Small always-on-top notification ("toast") shown in a screen corner.
// Never takes focus, so it does not disturb the app the user is typing in.

// Posted to the click target when the user clicks the toast.
#define WM_AI_TOAST_CLICKED	(WM_USER + 304)

enum class AiToastKind
{
	Info,
	Success,
	Error
};

class CAiToastWnd : public CWnd
{
public:
	CAiToastWnd();
	virtual ~CAiToastWnd();

	// timeoutMs == 0 keeps the toast visible until the next ShowToast/HideToast call.
	// position: 0 bottom right, 1 top right, 2 bottom left, 3 top left
	// clickHint adds a "click for details" line at the bottom.
	void ShowToast(const CString &title, const CString &body, AiToastKind kind, UINT timeoutMs, int position, bool clickHint = false);
	void HideToast(bool fade = true);

	void SetClickTarget(HWND hWnd) { m_clickTarget = hWnd; }

protected:
	DECLARE_MESSAGE_MAP()
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg int OnMouseActivate(CWnd *pDesktopWnd, UINT nHitTest, UINT message);

	bool EnsureCreated();
	void CreateFonts(int dpi);
	int Scale(int value) const { return MulDiv(value, m_dpi, 96); }
	CSize CalcLayout(CDC &dc, int width, CRect &titleRect, CRect &bodyRect, CRect &hintRect);

	CString m_title;
	CString m_body;
	AiToastKind m_kind;
	bool m_clickHint;
	HWND m_clickTarget;
	CRect m_hintRect;
	int m_dpi;
	int m_fontDpi;
	int m_alpha;
	CFont m_titleFont;
	CFont m_bodyFont;
	CRect m_titleRect;
	CRect m_bodyRect;
};
