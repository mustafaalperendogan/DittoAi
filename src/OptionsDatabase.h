#pragma once

#include "AiDialogs.h"
#include "DbSync.h"

// Options tab "Veritabanı": database path and maintenance, cloud sync settings.
// Built from an in-memory template because the resource script cannot hold Turkish text.

class COptionsDatabase : public CPropertyPage
{
	DECLARE_DYNAMIC(COptionsDatabase)

public:
	COptionsDatabase();
	virtual ~COptionsDatabase();

	virtual BOOL OnInitDialog();
	virtual BOOL OnApply();

protected:
	virtual void DoDataExchange(CDataExchange *pDX);

	DECLARE_MESSAGE_MAP()
	afx_msg void OnDestroy();
	afx_msg void OnBrowse();
	afx_msg void OnPathChanged();
	afx_msg void OnCompact();
	afx_msg void OnToggle();
	afx_msg void OnTest();
	afx_msg void OnSyncNow();
	afx_msg LRESULT OnSyncStatus(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnTestDone(WPARAM wParam, LPARAM lParam);

	void BuildTemplate();
	bool ReadSyncSettings(DbSyncSettings &settings);	// false = invalid number, message shown
	bool CheckSyncSettings(const DbSyncSettings &settings);
	bool ApplyDatabasePath();
	void UpdateEnabled();
	void ShowDbInfo();
	void ShowStatus();

	CAiDialogTemplate m_template;
	CMFCLinkCtrl m_envVarLink;
	CString m_envVarText;
};
