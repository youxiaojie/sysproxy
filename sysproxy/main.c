#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#include <Wininet.h>
#include <wchar.h>
#include <tchar.h>
#include <ras.h>
#include <raserror.h>

#pragma comment(lib, "rasapi32.lib")
#pragma comment(lib, "Wininet.lib")

enum RET_ERRORS
{
	RET_NO_ERROR = 0,
	INVALID_FORMAT = 1,
	NO_PERMISSION = 2,
	SYSCALL_FAILED = 3,
	NO_MEMORY = 4,
	INVAILD_OPTION_COUNT = 5,
};


void usage(LPCTSTR binName)
{
	_tprintf(_T("Usage: %s global <proxy-server> [<bypass-list>]\n"), binName);
	_tprintf(_T("             bypass list is a string like: localhost;127.*;10.* without trailing semicolon.\n"));
	_tprintf(_T("       %s pac <pac-url>\n"), binName);
	_tprintf(_T("       %s query\n"), binName);
	_tprintf(_T("       %s set <flags> [<proxy-server> [<bypass-list> [<pac-url>]]]\n"), binName);
	_tprintf(_T("             <flags> is bitwise combination of INTERNET_PER_CONN_FLAGS.\n"));
	_tprintf(_T("             \"-\" is a placeholder to keep the original value.\n"));

	exit(INVALID_FORMAT);
}

void reportWindowsError(LPCTSTR action)
{
	LPTSTR pErrMsg = NULL;
	DWORD errCode = GetLastError();
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ARGUMENT_ARRAY,
	                                            NULL, errCode, LANG_NEUTRAL, (LPTSTR)&pErrMsg, 0, NULL);
	_ftprintf(stderr, _T("Error %s: %lu %s\n"), action, errCode, pErrMsg);
}


void reportError(LPCTSTR action)
{
	_ftprintf(stderr, _T("Error %s\n"), action);
}


void initialize(INTERNET_PER_CONN_OPTION_LIST* options, int option_count)
{
	if (option_count < 1)
	{
		exit(INVAILD_OPTION_COUNT);
	}

	DWORD dwBufferSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
	options->dwSize = dwBufferSize;

	options->dwOptionCount = option_count;
	options->dwOptionError = 0;

	options->pOptions = calloc(option_count, sizeof(INTERNET_PER_CONN_OPTION));

	if (options->pOptions == NULL)
	{
		exit(NO_MEMORY);
	}
}


int apply_connect(INTERNET_PER_CONN_OPTION_LIST* options, LPTSTR conn)
{
	options->pszConnection = conn;

	BOOL result = InternetSetOption(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION, options, sizeof(INTERNET_PER_CONN_OPTION_LIST));
	if (!result)
	{
		reportWindowsError(_T("setting options"));
		return SYSCALL_FAILED;
	}

	result = InternetSetOption(NULL, INTERNET_OPTION_PROXY_SETTINGS_CHANGED, NULL, 0);
	if (!result)
	{
		reportWindowsError(_T("propagating changes"));
		return SYSCALL_FAILED;
	}

	result = InternetSetOption(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
	if (!result)
	{
		reportWindowsError(_T("refreshing"));
		return SYSCALL_FAILED;
	}

	return RET_NO_ERROR;
}


int apply(INTERNET_PER_CONN_OPTION_LIST* options)
{
	int ret;
	DWORD dwCb = 0;
	DWORD dwRet;
	DWORD dwEntries = 0;
	LPRASENTRYNAME lpRasEntryName = NULL;

	// Set LAN
	if ((ret = apply_connect(options, NULL)) > RET_NO_ERROR)
		goto free_calloc;

	// Find connections and apply proxy settings
	dwRet = RasEnumEntries(NULL, NULL, lpRasEntryName, &dwCb, &dwEntries);

	if (dwRet == ERROR_BUFFER_TOO_SMALL)
	{
		lpRasEntryName = (LPRASENTRYNAME)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwCb);

		if (lpRasEntryName == NULL)
		{
			reportError(_T("HeapAlloc"));
			ret = NO_MEMORY;
			goto free_calloc;
		}

		for (DWORD i = 0; i < dwEntries; i++)
		{
			lpRasEntryName[i].dwSize = sizeof(RASENTRYNAME);
		}

		dwRet = RasEnumEntries(NULL, NULL, lpRasEntryName, &dwCb, &dwEntries);
	}

	if (dwRet != ERROR_SUCCESS)
	{
		_ftprintf(stderr, _T("Error RasEnumEntries: %d\n"), dwRet);

		ret = SYSCALL_FAILED;
		goto free_ras;
	}
	else
	{
		if (dwEntries > 0)
		{
			for (DWORD i = 0; i < dwEntries; i++)
			{
				if ((ret = apply_connect(options, lpRasEntryName[i].szEntryName)) > RET_NO_ERROR)
					goto free_ras;
			}
		}
	}

	ret = RET_NO_ERROR;

free_ras:
	HeapFree(GetProcessHeap(), 0, lpRasEntryName);
	lpRasEntryName = NULL;
	/* fall through */
free_calloc:
	free(options->pOptions);
	options->pOptions = NULL;

	return ret;
}

int query(INTERNET_PER_CONN_OPTION_LIST* options)
{
	int ret;

	DWORD dwLen = sizeof(INTERNET_PER_CONN_OPTION_LIST);

	// On Windows 7 or above (IE8+), query with INTERNET_PER_CONN_FLAGS_UI is recommended.
	// See https://msdn.microsoft.com/en-us/library/windows/desktop/aa385145(v=vs.85).aspx
	options->pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS_UI;

	options->pOptions[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
	options->pOptions[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
	options->pOptions[3].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;

	if (!InternetQueryOption(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION, options, &dwLen))
	{
		// Set option to INTERNET_PER_CONN_FLAGS and try again to compatible with older versions of Windows.
		options->pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;

		if (!InternetQueryOption(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION, options, &dwLen))
		{
			reportWindowsError(_T("query options"));
			ret = SYSCALL_FAILED;
			goto free_calloc;
		}
	}

	_tprintf(_T("%d\n%s\n%s\n%s"),
		options->pOptions[0].Value.dwValue,
		options->pOptions[1].Value.pszValue,
		options->pOptions[2].Value.pszValue,
		options->pOptions[3].Value.pszValue);

	for (DWORD i = 1; i < options->dwOptionCount; ++i)
	{
		if (options->pOptions[i].Value.pszValue == NULL)
		{
			continue;
		}
		GlobalFree(options->pOptions[i].Value.pszValue);
		options->pOptions[i].Value.pszValue = NULL;
	}

	ret = RET_NO_ERROR;

free_calloc:
	free(options->pOptions);
	options->pOptions = NULL;

	return ret;
}

int _tmain(int argc, LPTSTR argv[])
{
#ifdef _UNICODE
	_setmode(_fileno(stdin), _O_U16TEXT);
	_setmode(_fileno(stdout), _O_U16TEXT);
	_setmode(_fileno(stderr), _O_U16TEXT);
#endif

	if (argc < 2)
	{
		usage(argv[0]);
	}

	INTERNET_PER_CONN_OPTION_LIST options;
	memset(&options, 0, sizeof(INTERNET_PER_CONN_OPTION_LIST));

	if (_tcscmp(argv[1], _T("global")) == 0 && argc >= 3)
	{
		if (argc > 4)
		{
			_tprintf(_T("Error: bypass list shouldn't contain spaces, please check parameters.\n"));
			usage(argv[0]);
		}

		initialize(&options, 3);

		options.pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;
		options.pOptions[0].Value.dwValue = PROXY_TYPE_PROXY | PROXY_TYPE_DIRECT;

		options.pOptions[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
		options.pOptions[1].Value.pszValue = argv[2];

		options.pOptions[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;

		if (argc == 4)
		{
			options.pOptions[2].Value.pszValue = argv[3];
		}
		else
		{
			options.pOptions[2].Value.pszValue = _T("<local>");
		}
	}
	else if (_tcscmp(argv[1], _T("pac")) == 0 && argc >= 3)
	{
		initialize(&options, 2);

		options.pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;
		options.pOptions[0].Value.dwValue = PROXY_TYPE_AUTO_PROXY_URL | PROXY_TYPE_DIRECT;

		options.pOptions[1].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
		options.pOptions[1].Value.pszValue = argv[2];
	}
	else if (_tcscmp(argv[1], _T("query")) == 0)
	{
		initialize(&options, 4);
		return query(&options);
	}
	else if (_tcscmp(argv[1], _T("set")) == 0)
	{
		if (argc > 6 || argc < 3)
		{
			usage(argv[0]);
		}

		DWORD flags = _ttoi(argv[2]);
		if (flags > 0x0f || flags < 1)
		{
			_tprintf(_T("Error: flags is not accepted\n"));
			usage(argv[0]);
		}

		DWORD count = argc - 2, argIdx = 2, optIdx = 1;
		DWORD opts[] = { 0, 0, INTERNET_PER_CONN_FLAGS,
			INTERNET_PER_CONN_PROXY_SERVER, INTERNET_PER_CONN_PROXY_BYPASS,
			INTERNET_PER_CONN_AUTOCONFIG_URL };

		initialize(&options, count);

		options.pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;
		options.pOptions[0].Value.dwValue = flags;

		while (argIdx++ <= count)
		{
			if (argv[argIdx][0] == _T('-'))
			{
				continue;
			}
			options.pOptions[optIdx].dwOption = opts[argIdx];
			options.pOptions[optIdx].Value.pszValue = argv[argIdx];
			++optIdx;
		}

		options.dwOptionCount = optIdx;
	}
	else
	{
		usage(argv[0]);
	}

	return apply(&options);
}
