#include "stdafx.h"
#include "CP_Main.h"
#include "OptionsDatabase.h"
#include "DatabaseUtilities.h"
#include "Misc.h"
#include "Options.h"

#include <memory>

namespace
{
	const WORD IDC_DB_PATH = 3001;
	const WORD IDC_DB_BROWSE = 3002;
	const WORD IDC_DB_ENVLINK = 3003;
	const WORD IDC_DB_INFO = 3004;
	const WORD IDC_DB_COMPACT = 3005;

	const WORD IDC_SYNC_ENABLED = 3010;
	const WORD IDC_SYNC_URL = 3011;
	const WORD IDC_SYNC_KEY = 3012;
	const WORD IDC_SYNC_PASSWORD = 3013;
	const WORD IDC_SYNC_AUTO = 3014;
	const WORD IDC_SYNC_INTERVAL = 3015;
	const WORD IDC_SYNC_ONSTART = 3016;
	const WORD IDC_SYNC_RICH = 3017;
	const WORD IDC_SYNC_MAXKB = 3018;
	const WORD IDC_SYNC_DELETES = 3019;
	const WORD IDC_SYNC_TEST = 3020;
	const WORD IDC_SYNC_NOW = 3021;
	const WORD IDC_SYNC_STATUS = 3022;

	const LPCTSTR CAPTION = _T("Ditto - Veritabanı");
	const LPCTSTR TEST_TEXT = _T("Bağlantıyı test et");
}

IMPLEMENT_DYNAMIC(COptionsDatabase, CPropertyPage)

COptionsDatabase::COptionsDatabase()
{
	BuildTemplate();
	m_psp.dwFlags |= PSP_DLGINDIRECT;
	m_psp.pResource = m_template.Get();
}

COptionsDatabase::~COptionsDatabase()
{
}

void COptionsDatabase::BuildTemplate()
{
	CAiDialogTemplate &t = m_template;
	t.Begin(L"Veritabanı", 375, 275);

	const short lx = 14, lw = 66, fx = 82;

	// ---- database
	t.Control((WORD)0x0080, L"Veritabanı", 0xFFFF, BS_GROUPBOX, 0, 7, 4, 361, 74);
	t.Label(L"Veritabanı yolu", lx, 19, lw);
	t.Edit(IDC_DB_PATH, fx, 17, 262);
	t.Button(IDC_DB_BROWSE, L"...", 348, 17, 14, 12);
	t.Control(L"Button", L"Ortam değişkenleri kullanılabilir (%APPDATA% gibi)", IDC_DB_ENVLINK, WS_TABSTOP | BS_PUSHBUTTON, 0, fx, 31, 262, 12);
	t.Control((WORD)0x0082, L"", IDC_DB_INFO, SS_LEFT | SS_NOPREFIX, 0, lx, 54, 250, 8);
	t.Button(IDC_DB_COMPACT, L"Sıkıştır ve onar", 284, 51, 78, 14);

	// ---- cloud sync
	t.Control((WORD)0x0080, L"Bulut senkronu", 0xFFFF, BS_GROUPBOX, 0, 7, 82, 361, 188);
	t.Check(IDC_SYNC_ENABLED, L"Veritabanını bulutla senkron tut", lx, 96, 250);
	t.Label(L"API adresi", lx, 113, lw);
	t.Edit(IDC_SYNC_URL, fx, 111, 280);
	t.Label(L"API anahtarı", lx, 129, lw);
	t.Edit(IDC_SYNC_KEY, fx, 127, 140, 12, ES_PASSWORD);
	t.Label(L"Şifreleme parolası", lx, 145, lw);
	t.Edit(IDC_SYNC_PASSWORD, fx, 143, 140, 12, ES_PASSWORD);
	t.Label(L"Tüm bilgisayarlarda aynı olmalı.", 228, 145, 134);
	t.Check(IDC_SYNC_AUTO, L"Otomatik senkronla, her", lx, 162, 96);
	t.Edit(IDC_SYNC_INTERVAL, 112, 161, 26, 12, ES_NUMBER);
	t.Label(L"dakikada bir", 142, 163, 80);
	t.Check(IDC_SYNC_ONSTART, L"Ditto açılınca senkronla", lx, 176, 250);
	t.Check(IDC_SYNC_RICH, L"Resim, RTF ve HTML biçimlerini de gönder", lx, 190, 178);
	t.Label(L"En büyük klip (KB)", 196, 191, 68);
	t.Edit(IDC_SYNC_MAXKB, 266, 189, 36, 12, ES_NUMBER);
	t.Check(IDC_SYNC_DELETES, L"Burada silinen klipleri diğer bilgisayarlardan da sil", lx, 204, 300);
	t.Button(IDC_SYNC_TEST, TEST_TEXT, lx, 220, 80, 14);
	t.Button(IDC_SYNC_NOW, L"Şimdi senkronla", 98, 220, 80, 14);
	t.Control((WORD)0x0082, L"", IDC_SYNC_STATUS, SS_LEFT | SS_NOPREFIX, 0, lx, 240, 348, 26);
}

void COptionsDatabase::DoDataExchange(CDataExchange *pDX)
{
	CPropertyPage::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_DB_ENVLINK, m_envVarLink);
}

BEGIN_MESSAGE_MAP(COptionsDatabase, CPropertyPage)
	ON_WM_DESTROY()
	ON_BN_CLICKED(IDC_DB_BROWSE, &COptionsDatabase::OnBrowse)
	ON_EN_CHANGE(IDC_DB_PATH, &COptionsDatabase::OnPathChanged)
	ON_BN_CLICKED(IDC_DB_COMPACT, &COptionsDatabase::OnCompact)
	ON_BN_CLICKED(IDC_SYNC_AUTO, &COptionsDatabase::OnToggle)
	ON_BN_CLICKED(IDC_SYNC_TEST, &COptionsDatabase::OnTest)
	ON_BN_CLICKED(IDC_SYNC_NOW, &COptionsDatabase::OnSyncNow)
	ON_MESSAGE(WM_DB_SYNC_STATUS, &COptionsDatabase::OnSyncStatus)
	ON_MESSAGE(WM_DB_SYNC_TEST_DONE, &COptionsDatabase::OnTestDone)
END_MESSAGE_MAP()

BOOL COptionsDatabase::OnInitDialog()
{
	CPropertyPage::OnInitDialog();

	m_envVarLink.SetURL(_T("https://github.com/sabrogden/Ditto/wiki/Environment-Variables"));
	m_envVarLink.GetWindowText(m_envVarText);

	// after the link text is known: EN_CHANGE shows the resolved path when the path has variables
	SetDlgItemText(IDC_DB_PATH, CGetSetOptions::GetDBPath(false));

	DbSyncSettings settings;
	settings.Load();
	CheckDlgButton(IDC_SYNC_ENABLED, settings.enabled ? BST_CHECKED : BST_UNCHECKED);
	SetDlgItemText(IDC_SYNC_URL, settings.url);
	SetDlgItemText(IDC_SYNC_KEY, settings.apiKey);
	SetDlgItemText(IDC_SYNC_PASSWORD, settings.password);
	CheckDlgButton(IDC_SYNC_AUTO, settings.autoSync ? BST_CHECKED : BST_UNCHECKED);
	SetDlgItemInt(IDC_SYNC_INTERVAL, settings.intervalMinutes, FALSE);
	CheckDlgButton(IDC_SYNC_ONSTART, settings.syncOnStart ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_SYNC_RICH, settings.includeRichFormats ? BST_CHECKED : BST_UNCHECKED);
	SetDlgItemInt(IDC_SYNC_MAXKB, settings.maxClipKB, FALSE);
	CheckDlgButton(IDC_SYNC_DELETES, settings.propagateDeletes ? BST_CHECKED : BST_UNCHECKED);

	UpdateEnabled();
	ShowDbInfo();

	CDbSync::Instance().SetStatusWindow(m_hWnd);
	ShowStatus();

	return TRUE;
}

void COptionsDatabase::OnDestroy()
{
	CDbSync::Instance().SetStatusWindow(NULL);
	CPropertyPage::OnDestroy();
}

BOOL COptionsDatabase::OnApply()
{
	DbSyncSettings settings;
	if (!ReadSyncSettings(settings) || (settings.enabled && !CheckSyncSettings(settings)))
	{
		return FALSE;
	}

	if (!ApplyDatabasePath())
	{
		return FALSE;
	}

	DbSyncSettings previous;
	previous.Load();
	settings.Save();
	CDbSync::Instance().SettingsChanged();
	if (settings.enabled && !previous.enabled)
	{
		CDbSync::Instance().SyncNow();
	}

	return CPropertyPage::OnApply();
}

// Moved here from the General tab, same behaviour.
bool COptionsDatabase::ApplyDatabasePath()
{
	CString toSavePath;
	GetDlgItemText(IDC_DB_PATH, toSavePath);
	CString resolvedPath = CGetSetOptions::ResolvePath(toSavePath);

	bool bOpenNewDatabase = false;

	if (resolvedPath.IsEmpty() == FALSE)
	{
		if (FileExists(resolvedPath) == FALSE)
		{
			CString cs;
			cs.Format(_T("%s veritabanı yok.\n\nYeni bir veritabanı oluşturulsun mu?"), (LPCTSTR)resolvedPath);

			if (MessageBox(cs, CAPTION, MB_OKCANCEL) == IDOK)
			{
				if (CreateDB(resolvedPath))
				{
					bOpenNewDatabase = true;
				}
				else
				{
					MessageBox(_T("Veritabanı oluşturulamadı."), CAPTION, MB_ICONWARNING);
				}
			}
			else
			{
				return false;
			}
		}
		else
		{
			if (ValidDB(resolvedPath) == FALSE)
			{
				MessageBox(_T("Geçersiz veritabanı."), CAPTION, MB_ICONWARNING);
				GetDlgItem(IDC_DB_PATH)->SetFocus();
				return false;
			}
			else
			{
				bOpenNewDatabase = true;
			}
		}

		if (bOpenNewDatabase)
		{
			CGetSetOptions::SetDBPath(toSavePath);

			if (OpenDatabase(resolvedPath) == FALSE)
			{
				MessageBox(_T("Yeni veritabanı açılamadı."), CAPTION, MB_ICONWARNING);
				GetDlgItem(IDC_DB_PATH)->SetFocus();
				return false;
			}
			else
			{
				theApp.RefreshView();
			}
		}
	}

	return true;
}

void COptionsDatabase::OnBrowse()
{
	OPENFILENAME FileName;
	TCHAR szFileName[400];
	TCHAR szDir[400];

	memset(&FileName, 0, sizeof(FileName));
	memset(szFileName, 0, sizeof(szFileName));
	memset(&szDir, 0, sizeof(szDir));
	FileName.lStructSize = sizeof(FileName);
	FileName.lpstrTitle = _T("Veritabanı aç");
	FileName.Flags = OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	FileName.nMaxFile = 400;
	FileName.lpstrFile = szFileName;
	FileName.lpstrInitialDir = szDir;
	FileName.lpstrFilter = _T("Ditto veritabanları (*.db; *.mdb)\0*.db;*.mdb\0\0");
	FileName.lpstrDefExt = _T("db");
	FileName.hwndOwner = m_hWnd;

	if (GetOpenFileName(&FileName) == 0)
	{
		return;
	}

	CString csPath(FileName.lpstrFile);
	if (FileExists(csPath) && ValidDB(csPath) == FALSE)
	{
		MessageBox(_T("Geçersiz veritabanı."), CAPTION, MB_ICONWARNING);
		GetDlgItem(IDC_DB_PATH)->SetFocus();
		return;
	}
	SetDlgItemText(IDC_DB_PATH, csPath);
}

void COptionsDatabase::OnPathChanged()
{
	if (m_envVarLink.m_hWnd == NULL)
	{
		return;
	}

	CString path;
	GetDlgItemText(IDC_DB_PATH, path);
	m_envVarLink.SetWindowText(path.Find(_T('%')) >= 0 ? CGetSetOptions::ResolvePath(path) : m_envVarText);
	m_envVarLink.Invalidate();
}

void COptionsDatabase::OnCompact()
{
	int ret = MessageBox(_T("Bu işlem uzun sürebilir ve veritabanı boyutunun iki katına kadar boş disk alanı gerektirir. Devam edilsin mi?"), CAPTION, MB_OKCANCEL | MB_ICONQUESTION);
	if (ret != IDOK)
	{
		return;
	}

	// same steps as Advanced > Compact and Repair
	CWaitCursor wait;
	try
	{
		try
		{
			for (int i = 0; i < 100; i++)
			{
				int toDeleteCount = theApp.m_db.execScalar(_T("SELECT COUNT(clipID) FROM MainDeletes"));
				if (toDeleteCount <= 0)
					break;

				RemoveOldEntries(false);
			}
		}
		CATCH_SQLITE_EXCEPTION

		theApp.m_db.execDML(_T("PRAGMA auto_vacuum = 1"));
		theApp.m_db.execQuery(_T("VACUUM"));
	}
	CATCH_SQLITE_EXCEPTION

	ShowDbInfo();
}

void COptionsDatabase::OnToggle()
{
	UpdateEnabled();
}

void COptionsDatabase::UpdateEnabled()
{
	GetDlgItem(IDC_SYNC_INTERVAL)->EnableWindow(IsDlgButtonChecked(IDC_SYNC_AUTO) == BST_CHECKED);
}

bool COptionsDatabase::ReadSyncSettings(DbSyncSettings &settings)
{
	settings.enabled = IsDlgButtonChecked(IDC_SYNC_ENABLED) == BST_CHECKED;
	GetDlgItemText(IDC_SYNC_URL, settings.url);
	settings.url.Trim();
	GetDlgItemText(IDC_SYNC_KEY, settings.apiKey);
	settings.apiKey.Trim();
	GetDlgItemText(IDC_SYNC_PASSWORD, settings.password);
	settings.autoSync = IsDlgButtonChecked(IDC_SYNC_AUTO) == BST_CHECKED;
	settings.syncOnStart = IsDlgButtonChecked(IDC_SYNC_ONSTART) == BST_CHECKED;
	settings.includeRichFormats = IsDlgButtonChecked(IDC_SYNC_RICH) == BST_CHECKED;
	settings.propagateDeletes = IsDlgButtonChecked(IDC_SYNC_DELETES) == BST_CHECKED;

	BOOL ok = FALSE;
	int interval = (int)GetDlgItemInt(IDC_SYNC_INTERVAL, &ok, FALSE);
	if (!ok || interval < 1 || interval > 1440)
	{
		MessageBox(_T("Senkron aralığı 1 ile 1440 dakika arasında olmalı."), CAPTION, MB_ICONWARNING);
		GetDlgItem(IDC_SYNC_INTERVAL)->SetFocus();
		return false;
	}
	settings.intervalMinutes = interval;

	int maxKB = (int)GetDlgItemInt(IDC_SYNC_MAXKB, &ok, FALSE);
	if (!ok || maxKB < 1 || maxKB > 102400)
	{
		MessageBox(_T("En büyük klip boyutu 1 ile 102400 KB arasında olmalı."), CAPTION, MB_ICONWARNING);
		GetDlgItem(IDC_SYNC_MAXKB)->SetFocus();
		return false;
	}
	settings.maxClipKB = maxKB;
	return true;
}

bool COptionsDatabase::CheckSyncSettings(const DbSyncSettings &settings)
{
	CString error = settings.Validate();
	if (error.IsEmpty())
	{
		return true;
	}
	MessageBox(error, CAPTION, MB_ICONWARNING);
	GetDlgItem(error.Find(_T("parola")) >= 0 ? IDC_SYNC_PASSWORD : IDC_SYNC_URL)->SetFocus();
	return false;
}

void COptionsDatabase::OnTest()
{
	DbSyncSettings settings;
	if (!ReadSyncSettings(settings) || !CheckSyncSettings(settings))
	{
		return;
	}
	GetDlgItem(IDC_SYNC_TEST)->EnableWindow(FALSE);
	SetDlgItemText(IDC_SYNC_TEST, _T("Test ediliyor..."));
	CDbSync::Instance().StartTest(settings, m_hWnd);
}

LRESULT COptionsDatabase::OnTestDone(WPARAM wParam, LPARAM lParam)
{
	std::unique_ptr<CString> message((CString *)lParam);
	SetDlgItemText(IDC_SYNC_TEST, TEST_TEXT);
	GetDlgItem(IDC_SYNC_TEST)->EnableWindow(TRUE);
	MessageBox(*message, CAPTION, wParam ? MB_ICONINFORMATION : MB_ICONWARNING);
	return 0;
}

void COptionsDatabase::OnSyncNow()
{
	DbSyncSettings settings;
	if (!ReadSyncSettings(settings) || !CheckSyncSettings(settings))
	{
		return;
	}
	// the worker reads the saved settings
	settings.Save();
	CDbSync::Instance().SettingsChanged();
	CDbSync::Instance().SyncNow();
	SetDlgItemText(IDC_SYNC_STATUS, _T("Ayarlar kaydedildi, senkron başlıyor..."));
}

LRESULT COptionsDatabase::OnSyncStatus(WPARAM, LPARAM)
{
	ShowStatus();
	ShowDbInfo();
	return 0;
}

void COptionsDatabase::ShowStatus()
{
	CString status;
	if (CDbSync::Instance().IsRunning())
	{
		status = _T("Senkron sürüyor...");
	}
	else
	{
		status = CDbSync::LastStatus();
		if (status.IsEmpty())
		{
			status = _T("Henüz senkron yapılmadı.");
		}
	}
	SetDlgItemText(IDC_SYNC_STATUS, status);
}

void COptionsDatabase::ShowDbInfo()
{
	CString text;
	try
	{
		double sizeMB = 0;
		WIN32_FILE_ATTRIBUTE_DATA data;
		if (::GetFileAttributesEx(CGetSetOptions::GetDBPath(), GetFileExInfoStandard, &data))
		{
			sizeMB = ((double)data.nFileSizeHigh * 4294967296.0 + data.nFileSizeLow) / (1024.0 * 1024.0);
		}
		int clips = theApp.m_db.execScalar(_T("SELECT COUNT(*) FROM Main WHERE bIsGroup = 0"));
		int groups = theApp.m_db.execScalar(_T("SELECT COUNT(*) FROM Main WHERE bIsGroup = 1"));

		CString size;
		size.Format(_T("%.1f"), sizeMB);
		size.Replace(_T('.'), _T(','));
		text.Format(_T("Boyut %s MB · %d klip · %d grup"), (LPCTSTR)size, clips, groups);
	}
	catch (CppSQLite3Exception &)
	{
		text = _T("Veritabanı bilgisi okunamadı.");
	}
	SetDlgItemText(IDC_DB_INFO, text);
}
