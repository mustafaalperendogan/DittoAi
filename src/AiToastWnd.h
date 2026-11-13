#pragma once

#include <memory>
#include <vector>

// Small always-on-top notifications ("toasts") shown in a screen corner.
// They never take focus, so they do not disturb the app the user is typing in.
// Several toasts can be visible at once; CAiToastStack lines them up so they never overlap.

// Posted to the click target when the user clicks a toast: wParam = toast key, lParam = 1 if it had the click hint.
#define WM_AI_TOAST_CLICKED	(WM_USER + 304)

enum class AiToastKind
{
	Info,
	Success,
	Error
};

class CAiToastStack;

class CAiToastWnd : public CWnd
{
public:
	explicit CAiToastWnd(CAiToastStack *stack);
	virtual ~CAiToastWnd();

	// Sets the content and size; the stack places the window. timeoutMs == 0 keeps it until hidden.
	// position: 0 bottom right, 1 top right, 2 bottom left, 3 top left
	// clickHint adds a "click for details" line at the bottom.
	bool SetContent(const CString &title, const CString &body, AiToastKind kind, UINT timeoutMs, int position, bool clickHint);
	void PlaceAt(int x, int y);
	void HideToast(bool fade = true);

	int Key() const { return m_key; }
	void SetKey(int key) { m_key = key; }
	bool IsShown() const { return m_shown; }
	UINT Order() const { return m_order; }
	HMONITOR Monitor() const { return m_monitor; }
	const CRect &WorkArea() const { return m_work; }
	int Position() const { return m_position; }
	CSize Size() const { return m_size; }
	int Scale(int value) const { return MulDiv(value, m_dpi, 96); }

protected:
	DECLARE_MESSAGE_MAP()
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg int OnMouseActivate(CWnd *pDesktopWnd, UINT nHitTest, UINT message);

	bool EnsureCreated();
	void CreateFonts(int dpi);
	CSize CalcLayout(CDC &dc, int width, CRect &titleRect, CRect &bodyRect, CRect &hintRect);
	void HideNow(bool notifyStack);

	friend class CAiToastStack;

	CAiToastStack *m_stack;
	int m_key;
	bool m_shown;			// visible or fading out; it holds its place in the stack until hidden
	UINT m_order;			// when it was shown, older toasts sit nearer the corner
	HMONITOR m_monitor;
	CRect m_work;
	int m_position;
	CSize m_size;

	CString m_title;
	CString m_body;
	AiToastKind m_kind;
	bool m_clickHint;
	CRect m_hintRect;
	int m_dpi;
	int m_fontDpi;
	int m_alpha;
	CFont m_titleFont;
	CFont m_bodyFont;
	CRect m_titleRect;
	CRect m_bodyRect;
};

// Owns the toast windows. Each toast has a key: showing a key that is already visible
// updates that toast in place, other keys get their own toast stacked next to it.
class CAiToastStack
{
public:
	CAiToastStack() {}
	~CAiToastStack() { DestroyAll(); }

	void SetClickTarget(HWND hWnd) { m_clickTarget = hWnd; }

	void Show(int key, const CString &title, const CString &body, AiToastKind kind, UINT timeoutMs, int position, bool clickHint = false);
	void Hide(int key, bool fade = true);
	void DestroyAll();

private:
	friend class CAiToastWnd;

	void Layout();
	void OnToastHidden() { Layout(); }
	void OnToastClicked(int key, bool clickHint);

	std::vector<std::unique_ptr<CAiToastWnd>> m_toasts;	// hidden ones are reused
	HWND m_clickTarget = NULL;
	UINT m_nextOrder = 0;
	bool m_inLayout = false;
};
