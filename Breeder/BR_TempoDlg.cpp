/******************************************************************************
/ BR_TempoDlg.cpp
/
/ Copyright (c) 2013 Dominik Martin Drzic
/ http://forum.cockos.com/member.php?u=27094
/ http://www.standingwaterstudios.com/reaper
/
/ Permission is hereby granted, free of charge, to any person obtaining a copy
/ of this software and associated documentation files (the "Software"), to deal
/ in the Software without restriction, including without limitation the rights to
/ use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
/ of the Software, and to permit persons to whom the Software is furnished to
/ do so, subject to the following conditions:
/
/ The above copyright notice and this permission notice shall be included in all
/ copies or substantial portions of the Software.
/
/ THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
/ EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
/ OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
/ NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
/ HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
/ WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
/ FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
/ OTHER DEALINGS IN THE SOFTWARE.
/
******************************************************************************/
#include "stdafx.h"
#include "BR_TempoDlg.h"
#include "BR_EnvTools.h"
#include "BR_Tempo.h"
#include "BR_Util.h"
#include "../MarkerList/MarkerListActions.h"
#include "../SnM/SnM_Dlg.h"
#include "../reaper/localize.h"

/******************************************************************************
* Globals                                                                     *
******************************************************************************/
static double g_tempoShapeSplitRatio = -1;
static int g_tempoShapeSplitMiddle = -1;

/******************************************************************************
* Dialog state keys                                                           *
******************************************************************************/
const char* const SHAPE_KEY   = "BR - ChangeTempoShape";
const char* const SHAPE_WND   = "BR - ChangeTempoShape WndPos";
const char* const RAND_KEY    = "BR - RandomizeTempo";
const char* const RAND_WND    = "BR - RandomizeTempo WndPos";
const char* const CONVERT_KEY = "BR - ConvertTempo";
const char* const CONVERT_WND = "BR - ConvertTempo WndPos";
const char* const SEL_ADJ_KEY = "BR - SelectAdjustTempo";
const char* const SEL_ADJ_WND = "BR - SelectAdjustTempo WndPos";
const char* const UNSEL_KEY   = "BR - DeselectNthTempo";
const char* const UNSEL_WND   = "BR - DeselectNthTempo WndPos";

/******************************************************************************
* Convert project markers to tempo markers                                    *
******************************************************************************/
static void ConvertMarkersToTempo (int markers, int num, int den, bool removeMarkers, bool timeSel, bool gradualTempo, bool split, double splitRatio)
{
	vector<double> markerPositions = GetProjectMarkers(timeSel);

	// Check number of markers
	if (markerPositions.size() <=1)
	{
		if (timeSel)
			MessageBox(g_hwndParent, __LOCALIZE("Not enough project markers in the time selection to perform conversion.", "sws_DLG_166"), __LOCALIZE("SWS - Error", "sws_mbox"), MB_OK);
		else
			MessageBox(g_hwndParent, __LOCALIZE("Not enough project markers in the project to perform conversion.", "sws_DLG_166"), __LOCALIZE("SWS - Error", "sws_mbox"), MB_OK);
			return;
	}

	// Check if there are existing tempo markers after the first project marker
	if (CountTempoTimeSigMarkers(NULL) > 0)
	{
		bool check = false;
		int count = CountTempoTimeSigMarkers(NULL);
		for (int i = 0; i < count; ++i)
		{
			double position;
			GetTempoTimeSigMarker(NULL, i, &position, NULL, NULL, NULL, NULL, NULL, NULL);

			// Check for tempo markers between project markers
			if (!check && position > markerPositions.front() && position <= markerPositions.back())
			{
				int answer = MessageBox(g_hwndParent, __LOCALIZE("Detected existing tempo markers between project markers.\nAre you sure you want to continue? Strange things may happen.","sws_DLG_166"), __LOCALIZE("SWS - Warning","sws_mbox"), MB_YESNO);
				if (answer == 7)
					return;
				if (answer == 6)
					check = true;
			}

			// Check for tempo markers after last project marker
			if (position > markerPositions.back())
			{
				// When tempo envelope timebase is time, these tempo markers can't be moved so there is no need to warn user
				int timeBase; GetConfig("tempoenvtimelock", timeBase);
				if (timeBase == 0)
					break;
				else
				{
					int answer = MessageBox(g_hwndParent, __LOCALIZE("Detected existing tempo markers after the last project marker.\nThey may get moved during the conversion. Are you sure you want to continue?","sws_DLG_166"), __LOCALIZE("SWS - Warning","sws_mbox"), MB_YESNO);
					if (answer == 7)
						return;
					if (answer == 6)
						break;
				}
			}
		}
	}

	// If all went well start converting
	Undo_BeginBlock2(NULL);
	double measure = num / (den * (double)markers);
	int exceed = 0;

	// Square points
	if (!gradualTempo)
	{
		for (size_t i = 0; i < markerPositions.size()-1 ; ++i)
		{
			double bpm = (240 * measure) / (markerPositions[i+1] - markerPositions[i]);
			if (bpm > MAX_BPM)
				++exceed;

			if (i == 0) // Set first tempo marker with time signature
				SetTempoTimeSigMarker (NULL, -1, markerPositions[0], -1, -1, bpm, num, den, false);
			else
				SetTempoTimeSigMarker (NULL, -1, markerPositions[i], -1, -1, bpm, 0, 0, false);
		}
	}

	// Linear points
	else
	{
		// Get linear points' BPM (these get put where project markers are)
		vector <double> linearPoints;
		double prevBpm;
		for (size_t i = 0; i < markerPositions.size()-1; ++i)
		{
			double bpm = (240 * measure) / (markerPositions[i+1] - markerPositions[i]);

			// First marker is the same as square point
			if (i == 0)
				linearPoints.push_back(bpm);

			// Markers in between are the average of the current and previous transition
			else if (i != markerPositions.size()-2)
				linearPoints.push_back((bpm + prevBpm) / 2);

			// Last two (or one - depending on time selection)
			else
			{
				// for time selection:    last-1 is same as square,  last is ignored
				// for the whole project: last-1 is averaged,        last is same as square
				if (!timeSel)
					linearPoints.push_back((bpm + prevBpm) / 2);
				linearPoints.push_back(bpm);
			}
			prevBpm = bpm;
		}

		// Musical distance between starting tempo point and first middle point (used for checking at the end)
		double midMeasure = (split) ? (measure*(1-splitRatio) / 2) : (measure/2);

		// Set points
		for(size_t i = 0; i < linearPoints.size(); ++i)
		{
			// First tempo marker has time signature, last will have square shape if converting within time selection
			if (i == 0)
				SetTempoTimeSigMarker(NULL, -1, markerPositions[i], -1, -1, linearPoints[i], num, den, true);
			else if (i != linearPoints.size()-1)
				SetTempoTimeSigMarker(NULL, -1, markerPositions[i], -1, -1, linearPoints[i], 0, 0, true);
			else
			{
				if (timeSel)
					SetTempoTimeSigMarker(NULL, -1, markerPositions[i], -1, -1, linearPoints[i], 0, 0, false);
				else
					SetTempoTimeSigMarker(NULL, -1, markerPositions[i], -1, -1, linearPoints[i], 0, 0, true);
			}

			if (linearPoints[i] > MAX_BPM)
				++exceed;

			// Create middle point(s)
			if (i != linearPoints.size()-1)
			{
				// Get middle point's position and BPM
				double pos, bpm;
				FindMiddlePoint(&pos, &bpm, measure, markerPositions[i], markerPositions[i+1], linearPoints[i], linearPoints[i+1]);

				// Set middle point
				if (!split)
				{
					SetTempoTimeSigMarker(NULL, -1, pos, -1, -1, bpm, 0, 0, true);
					if (bpm > MAX_BPM)
						++exceed;
				}

				// Or split it
				else
				{
					double pos1, pos2, bpm1, bpm2;
					SplitMiddlePoint (&pos1, &pos2, &bpm1, &bpm2, splitRatio, measure, markerPositions[i], pos, markerPositions[i+1], linearPoints[i], bpm, linearPoints[i+1]);

					SetTempoTimeSigMarker(NULL, -1, pos1, -1, -1, bpm1, 0, 0, true);
					SetTempoTimeSigMarker(NULL, -1, pos2, -1, -1, bpm2, 0, 0, true);
					if (bpm1 > MAX_BPM || bpm2 > MAX_BPM)
						++exceed;

					pos = pos2; // used for checking at the end
					bpm = bpm2;
				}

				// Middle point's BPM  is always calculated in a relation to the point behind so it will land on the correct musical position.
				// But it can also make the end point move from it's designated musical position due to rounding errors (even if small at the
				// beginning, they can accumulate). That's why we recalculate next point's BPM (in a relation to last middle point) so it always lands
				// on the correct musical position
				linearPoints[i+1] = (480*midMeasure) / (markerPositions[i+1]-pos) - bpm;
			}
		}
	}

	// Remove markers
	if (removeMarkers)
	{
		if (timeSel)
			Main_OnCommand(40420, 0);
		else
			DeleteAllMarkers();
	}

	UpdateTimeline();
	Undo_EndBlock2 (NULL, __LOCALIZE("Convert project markers to tempo markers","sws_undo"), UNDO_STATE_ALL);

	// Warn user if there were tempo markers created with a BPM over 960
	if (exceed != 0)
		ShowMessageBox(__LOCALIZE("Some of the created tempo markers have a BPM over 960. If you try to edit them, they will revert back to 960 or lower.\n\nIt is recommended that you undo, edit project markers and try again.", "sws_DLG_166"),__LOCALIZE("SWS - Warning", "sws_mbox"), 0);
}

static void ShowGradualOptions (bool show, HWND hwnd)
{
	int c;
	if (show)
	{
		ShowWindow(GetDlgItem(hwnd, IDC_BR_CON_SPLIT), SW_SHOW);
		ShowWindow(GetDlgItem(hwnd, IDC_BR_CON_SPLIT_RATIO), SW_SHOW);
		c = 29;
	}

	else
	{
		ShowWindow(GetDlgItem(hwnd, IDC_BR_CON_SPLIT), SW_HIDE);
		ShowWindow(GetDlgItem(hwnd, IDC_BR_CON_SPLIT_RATIO), SW_HIDE);
		c = -29;
	}

	RECT r;

	// Move/resize group boxes
	GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_GROUP2), &r);
	SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_GROUP2), HWND_BOTTOM, 0, 0, r.right-r.left, r.bottom-r.top+c, SWP_NOMOVE);

	// Move buttons
	GetWindowRect(GetDlgItem(hwnd, IDOK), &r); ScreenToClient(hwnd, (LPPOINT)&r);
	SetWindowPos(GetDlgItem(hwnd, IDOK), NULL, r.left, r.top+c, 0, 0, SWP_NOSIZE);
	GetWindowRect(GetDlgItem(hwnd, IDCANCEL), &r); ScreenToClient(hwnd, (LPPOINT)&r);
	SetWindowPos(GetDlgItem(hwnd, IDCANCEL), NULL, r.left, r.top+c, 0, 0, SWP_NOSIZE);

	// Resize window
	GetWindowRect(hwnd, &r);
	#ifdef _WIN32
		SetWindowPos(hwnd, NULL, 0, 0, r.right-r.left, r.bottom-r.top+c, SWP_NOMOVE);
	#else
		SetWindowPos(hwnd, NULL, r.left, r.top-c,  r.right-r.left, r.bottom-r.top+c, NULL);
	#endif
}

static void SaveOptionsConversion (HWND hwnd)
{
	char eNum[128], eDen[128], eMarkers[128], splitRatio[128];
	GetDlgItemText(hwnd, IDC_BR_CON_NUM, eNum, 128);
	GetDlgItemText(hwnd, IDC_BR_CON_DEN, eDen, 128);
	GetDlgItemText(hwnd, IDC_BR_CON_MARKERS, eMarkers, 128);
	GetDlgItemText(hwnd, IDC_BR_CON_SPLIT_RATIO, splitRatio, 128);
	int num = atoi(eNum);
	int den = atoi(eDen);
	int markers = atoi(eMarkers);
	int removeMarkers = IsDlgButtonChecked(hwnd, IDC_BR_CON_REMOVE);
	int timeSel = IsDlgButtonChecked(hwnd, IDC_BR_CON_TIMESEL);
	int gradual = IsDlgButtonChecked(hwnd, IDC_BR_CON_GRADUAL);
	int split = IsDlgButtonChecked(hwnd, IDC_BR_CON_SPLIT);

	char tmp[256];
	_snprintf(tmp, sizeof(tmp), "%d %d %d %d %d %d %d %s", markers, num, den, removeMarkers, timeSel, gradual, split, splitRatio);
	WritePrivateProfileString("SWS", CONVERT_KEY, tmp, get_ini_file());
}

static void LoadOptionsConversion (int& markers, int& num, int& den, int& removeMarkers, int& timeSel, int& gradual, int& split, char* splitRatio)
{
	char tmp[256];
	GetPrivateProfileString("SWS", CONVERT_KEY, "4 4 4 1 0 0 0 4/8", tmp, 256, get_ini_file());
	sscanf(tmp, "%d %d %d %d %d %d %d %s", &markers, &num, &den, &removeMarkers, &timeSel, &gradual, &split, splitRatio);

	// Restore defaults if needed
	double convertedRatio;
	IsFraction (splitRatio, convertedRatio);
	if (convertedRatio <= 0 || convertedRatio >= 1)
		strcpy(splitRatio, "0");
	if (markers <= 0)
		markers = 4;
	if (num < MIN_SIG || num > MAX_SIG)
		num = 4;
	if (den < MIN_SIG || den > MAX_SIG)
		den = 4;
	if (removeMarkers != 0 && removeMarkers != 1)
		removeMarkers = 1;
	if (timeSel != 0 && timeSel != 1)
		timeSel = 0;
	if (gradual != 0 && gradual != 1)
		gradual = 0;
	if (split != 0 && split != 1)
		split = 0;
}

WDL_DLGRET ConvertMarkersToTempoProc (HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (INT_PTR r = SNM_HookThemeColorsMessage(hwnd, uMsg, wParam, lParam))
		return r;

	switch(uMsg)
	{
		case WM_INITDIALOG:
		{
			// Drop down
			WDL_UTF8_HookComboBox(GetDlgItem(hwnd, IDC_BR_CON_SPLIT_RATIO));
			int x = (int)SendDlgItemMessage(hwnd, IDC_BR_CON_SPLIT_RATIO, CB_ADDSTRING, 0, (LPARAM)"1/2");
			SendDlgItemMessage(hwnd, IDC_BR_CON_SPLIT_RATIO, CB_SETITEMDATA, x, 0);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_CON_SPLIT_RATIO, CB_ADDSTRING, 0, (LPARAM)"1/3");
			SendDlgItemMessage(hwnd, IDC_BR_CON_SPLIT_RATIO, CB_SETITEMDATA, x, 1);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_CON_SPLIT_RATIO, CB_ADDSTRING, 0, (LPARAM)"1/4");
			SendDlgItemMessage(hwnd, IDC_BR_CON_SPLIT_RATIO, CB_SETITEMDATA, x, 2);

			// Load options from .ini
			int markers, num, den, removeMarkers, timeSel, gradual, split;
			char eNum[128], eDen[128] , eMarkers[128], splitRatio[128];
			LoadOptionsConversion(markers, num, den, removeMarkers, timeSel, gradual, split, splitRatio);
			sprintf(eNum, "%d", num);
			sprintf(eDen, "%d", den);
			sprintf(eMarkers, "%d", markers);

			// Set controls
			SetDlgItemText(hwnd, IDC_BR_CON_MARKERS, eMarkers);
			SetDlgItemText(hwnd, IDC_BR_CON_NUM, eNum);
			SetDlgItemText(hwnd, IDC_BR_CON_DEN, eDen);
			SetDlgItemText(hwnd, IDC_BR_CON_SPLIT_RATIO, splitRatio);
			CheckDlgButton(hwnd, IDC_BR_CON_REMOVE, !!removeMarkers);
			CheckDlgButton(hwnd, IDC_BR_CON_TIMESEL, !!timeSel);
			CheckDlgButton(hwnd, IDC_BR_CON_GRADUAL, !!gradual);
			CheckDlgButton(hwnd, IDC_BR_CON_SPLIT, !!split);
			EnableWindow(GetDlgItem(hwnd, IDC_BR_CON_SPLIT), !!gradual);
			EnableWindow(GetDlgItem(hwnd, IDC_BR_CON_SPLIT_RATIO), !!split);
			#ifndef _WIN32
				RECT r;
				int c = 4;
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_MARKERS_STATIC), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_MARKERS_STATIC), HWND_BOTTOM, r.left, r.top+c, 0, 0, SWP_NOSIZE);
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_SIG_STATIC1), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_SIG_STATIC1), HWND_BOTTOM, r.left, r.top+c, 0, 0, SWP_NOSIZE);
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_SIG_STATIC2), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_SIG_STATIC2), HWND_BOTTOM, r.left, r.top+c, 0, 0, SWP_NOSIZE);
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_MARKERS), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_MARKERS), HWND_TOP, r.left, r.top+c, 0, 0, SWP_NOSIZE);
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_NUM), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_NUM), HWND_TOP, r.left, r.top+c, 0, 0, SWP_NOSIZE);
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_DEN), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_DEN), HWND_TOP, r.left, r.top+c, 0, 0, SWP_NOSIZE);
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_GRADUAL), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_GRADUAL), HWND_TOP, r.left, r.top+c, 0, 0, SWP_NOSIZE);
				c = 3;
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_REMOVE), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_REMOVE), HWND_TOP, r.left, r.top+c, 0, 0, SWP_NOSIZE);
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_TIMESEL), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_TIMESEL), HWND_TOP, r.left, r.top+c, 0, 0, SWP_NOSIZE);
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_SPLIT), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_SPLIT), HWND_TOP, r.left, r.top+c, 0, 0, SWP_NOSIZE);
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_CON_SPLIT_RATIO), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_CON_SPLIT_RATIO), HWND_TOP, r.left, r.top+c, 0, 0, SWP_NOSIZE);
			#endif

			if (!gradual)
				ShowGradualOptions(false, hwnd);
			RestoreWindowPos(hwnd, CONVERT_WND, false);
		}
		break;

		case WM_COMMAND :
		{
			switch(LOWORD(wParam))
			{
				case IDOK:
				{
					// Edit boxes and dropdown
					char eNum[128], eDen[128], eMarkers[128], splitRatio[128];
					double convertedSplitRatio;
					GetDlgItemText(hwnd, IDC_BR_CON_NUM, eNum, 128);
					GetDlgItemText(hwnd, IDC_BR_CON_DEN, eDen, 128);
					GetDlgItemText(hwnd, IDC_BR_CON_MARKERS, eMarkers, 128);
					GetDlgItemText(hwnd, IDC_BR_CON_SPLIT_RATIO, splitRatio, 128);

					int num = atoi(eNum); if (num > MAX_SIG){num = MAX_SIG;}
					int den = atoi(eDen); if (den > MAX_SIG){den = MAX_SIG;}

					int markers = atoi(eMarkers);
					IsFraction(splitRatio, convertedSplitRatio);
					if (convertedSplitRatio <= 0 || convertedSplitRatio >= 1)
					{
						convertedSplitRatio = 0;
						strcpy(splitRatio, "0");
					}

					// Check boxes
					bool removeMarkers = !!IsDlgButtonChecked(hwnd, IDC_BR_CON_REMOVE);
					bool timeSel = !!IsDlgButtonChecked(hwnd, IDC_BR_CON_TIMESEL);
					bool gradual = !!IsDlgButtonChecked(hwnd, IDC_BR_CON_GRADUAL);
					bool split = !!IsDlgButtonChecked(hwnd, IDC_BR_CON_SPLIT);

					// Update edit boxes and dropdown to show "atoied" value
					sprintf(eNum, "%d", num);
					sprintf(eDen, "%d", den);
					sprintf(eMarkers, "%d", markers);
					SetDlgItemText(hwnd, IDC_BR_CON_NUM, eNum);
					SetDlgItemText(hwnd, IDC_BR_CON_DEN, eDen);
					SetDlgItemText(hwnd, IDC_BR_CON_MARKERS, eMarkers);
					SetDlgItemText(hwnd, IDC_BR_CON_SPLIT_RATIO, splitRatio);

					// Check values
					if (markers <= 0 || num <= 0 || den <= 0)
					{
						MessageBox(g_hwndParent, __LOCALIZE("All values have to be positive integers.","sws_DLG_166"), __LOCALIZE("SWS - Error","sws_mbox"), MB_OK);

						if (markers <= 0)
						{
							SetFocus(GetDlgItem(hwnd, IDC_BR_CON_MARKERS));
							SendMessage(GetDlgItem(hwnd, IDC_BR_CON_MARKERS), EM_SETSEL, 0, -1);
						}
						else if (num <= 0)
						{
							SetFocus(GetDlgItem(hwnd, IDC_BR_CON_NUM));
							SendMessage(GetDlgItem(hwnd, IDC_BR_CON_NUM), EM_SETSEL, 0, -1);
						}
						else if (den <= 0)
						{
							SetFocus(GetDlgItem(hwnd, IDC_BR_CON_DEN));
							SendMessage(GetDlgItem(hwnd, IDC_BR_CON_DEN), EM_SETSEL, 0, -1);
						}
					}
					// If all went well, start converting
					else
					{
						if (convertedSplitRatio == 0)
							split = false;
						if (!IsWindowEnabled(GetDlgItem(hwnd, IDC_BR_CON_TIMESEL)))
							timeSel = false;
						ConvertMarkersToTempo(markers, num, den, removeMarkers, timeSel, gradual, split, convertedSplitRatio);
					}
				}
				break;

				case IDC_BR_CON_GRADUAL:
				{
					int gradual = IsDlgButtonChecked(hwnd, IDC_BR_CON_GRADUAL);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_CON_SPLIT), !!gradual);
					ShowGradualOptions(!!gradual, hwnd);
				}
				break;

				case IDC_BR_CON_SPLIT:
				{
					int split = IsDlgButtonChecked(hwnd, IDC_BR_CON_SPLIT);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_CON_SPLIT_RATIO), !!split);
				}
				break;

				case IDCANCEL:
				{
					ConvertMarkersToTempoDialog(NULL);
				}
				break;
			}
		}
		break;

		case WM_TIMER:
		{
			double tStart, tEnd;
			GetSet_LoopTimeRange2(NULL, false, false, &tStart, &tEnd, false);
			if (tStart == tEnd)
				EnableWindow(GetDlgItem(hwnd, IDC_BR_CON_TIMESEL), false);
			else
				EnableWindow(GetDlgItem(hwnd, IDC_BR_CON_TIMESEL), true);
		}
		break;

		case WM_DESTROY:
		{
			SaveWindowPos(hwnd, CONVERT_WND);
			SaveOptionsConversion (hwnd);
		}
		break;
	}
	return 0;
}

/******************************************************************************
* Select and adjust tempo markers                                             *
******************************************************************************/
static void SelectTempo (int mode, int Nth, int timeSel, int bpm, double bpmStart, double bpmEnd, int shape, int sig, int num, int den, int type)
{
	/*
	mode     ---> Ignore criteria
	               --> 0 to clear
	               --> 1 to invert selection
	               --> 2 to unselect every nth selected point

	         ---> Check by criteria before selecting
	               --> 3 to set new
	               --> 4 to add to existing selection
	               --> 5 to unselect
	               --> 6 to unselect every Nth selected marker
	               --> 7 to invert

	criteria ---> Nth     ---> ordinal number of the selected point
	         ---> timeSel ---> 0 for all, 1 for time selection, 2 to exclude time selection
	         ---> bpm     ---> 0 to ignore BPM, 1 to compare with bpmStart and bpmEnd
	         ---> shape   ---> 0 for all, 1 for square, 2 for linear
	         ---> sig     ---> 0 to ignore sig, if 1 num and den need to be specified
	         ---> type    ---> 0 for all, 1 for tempo markers only, 2 for time signature only
	*/

	// Get time selection
	double tStart, tEnd;
	GetSet_LoopTimeRange2 (NULL, false, false, &tStart, &tEnd, false);

	// Get tempo chunk
	TrackEnvelope* envelope = GetTempoEnv();
	char* envState = GetSetObjectState(envelope, "");
	char* token = strtok(envState, "\n");

	// Loop tempo chunk searching for tempo markers
	WDL_FastString newState;
	int currentNth = 1;
	LineParser lp(false);
	while (token != NULL)
	{
		lp.parse(token);
		BR_EnvPoint point;
		if (point.ReadLine(lp))
		{
			// Clear selected points
			if (mode == 0)
				point.selected = 0;

			// Invert selected points
			else if (mode == 1)
				point.selected = !point.selected;

			// Unselect every Nth selected point
			else if (mode == 2)
			{
				if (point.selected == 1)
				{
					if (currentNth == Nth)
						point.selected = 0;

					// Update current Nth
					++currentNth;
					if (currentNth > Nth)
						currentNth = 1;
				}
			}

			// Select/Unselect points by criteria
			else
			{
				//// Check point by criteria
				///////////////////////////////////////////////////////////////////////////////////////
				bool selectPt = true;

				// Check BPM
				if (selectPt && bpm)
					selectPt = (point.value >= bpmStart && point.value <= bpmEnd);

				// Check time signature
				if (selectPt && sig)
				{
					int effNum, effDen;
					TimeMap_GetTimeSigAtTime(NULL, point.position, &effNum, &effDen, NULL);
					selectPt = (num == effNum && den == effDen);
				}

				// Check time
				if (selectPt && timeSel)
				{
					selectPt = (point.position >= tStart && point.position <= tEnd);
					if (timeSel == 2)
						selectPt = !selectPt;
				}

				// Check shape
				if (selectPt && shape)
				{
					if (shape == 1)
						selectPt = (point.shape == 1);
					else if (shape == 2)
						selectPt = (point.shape == 0);
				}

				// Check type
				if (selectPt && type)
				{
					selectPt = (point.sig == 0);
					if (type == 2)
						selectPt = !selectPt;
				}

				//// Depending on the mode, designate point for selection/unselection
				///////////////////////////////////////////////////////////////////////////////////////

				// Mode "add to selection" - no matter the upper criteria, selected point stays selected
				if (mode == 4)
					selectPt = (point.selected) ? (true) : (selectPt);

				// Mode "unselect while obeying criteria" - unselected points stay that way...others get checked by criteria
				else if (mode == 5)
					selectPt = (point.selected) ? (!selectPt) : (false);

				// Mode "unselect every Nth selected marker while obeying criteria"
				else if (mode == 6)
				{
					if (selectPt)
					{
						if (point.selected == 1)
						{
							if (currentNth == Nth)
								selectPt = false;

							// Update current Nth
							++currentNth;
							if (currentNth > Nth)
								currentNth = 1;
						}
						else
							selectPt = false;
					}
					else
						selectPt = !!point.selected;
				}

				// Mode "invert while obeying criteria" - check if point passed criteria and then invert selection
				else if (mode == 7)
					selectPt = (selectPt) ? (!point.selected) : (!!point.selected);

				/// Finally select or unselect the point
				///////////////////////////////////////////////////////////////////////////////////////
				point.selected = (selectPt) ? (1) : (0);
			}

			// Update tempo point
			point.Append(newState);
		}

		else
		{
			newState.Append(token);
			newState.Append("\n");
		}
		token = strtok(NULL, "\n");
	}

	// Update tempo chunk
	GetSetObjectState(envelope, newState.Get());
	FreeHeapPtr(envState);
}

static void AdjustTempo (int mode, double bpm, int shape)
{
	/*
	mode ---> 0 for value, 1 for percentage
	shape --> 0 to ignore, 1 to invert, 2 for linear, 3 for square
	*/

	// Get tempo chunk
	TrackEnvelope* envelope = GetTempoEnv();
	char* envState = GetSetObjectState(envelope, "");
	char* token = strtok(envState, "\n");

	// Get TEMPO MARKERS timebase (not project timebase)
	int timeBase; GetConfig("tempoenvtimelock", timeBase);

	// Temporary change of preferences (prevent reselection of points in time selection)
	int envClickSegMode; GetConfig("envclicksegmode", envClickSegMode);
	SetConfig("envclicksegmode", ClearBit (envClickSegMode, 6));

	// Prepare variables
	double pTime; GetTempoTimeSigMarker(NULL, 0, &pTime, NULL, NULL, NULL, NULL, NULL, NULL);
	double pBpm = 1;
	int pShape = 1;
	double pOldTime = pTime;
	double pOldBpm = 1;
	int pOldShape = 1;

	// Loop through tempo chunk and perform BPM calculations
	WDL_FastString newState;
	LineParser lp(false);
	while(token != NULL)
	{
		lp.parse(token);
		BR_EnvPoint point;
		if (point.ReadLine(lp))
		{
			// Save "soon to be old" values
			double oldTime = point.position;
			double oldBpm = point.value;
			int oldShape = point.shape;

			// If point is selected calculate it's new BPM and shape.
			if (point.selected)
			{
				// Calculate BPM
				if (mode == 0)
					point.value += bpm;
				else
					point.value *= 1 + bpm/100;

				// Check if BPM is legal
				if (point.value < MIN_BPM)
					point.value = MIN_BPM;
				else if (point.value > MAX_BPM)
					point.value = MAX_BPM;

				// Set shape
				if (shape == 3)
					point.shape = 1;
				else if (shape == 2)
					point.shape = 0;
				else if (shape == 1)
				{
					if (point.shape == 1)
						point.shape = 0;
					else
						point.shape = 1;
				}
			}

			// Get new position but only if timebase is beats
			if (timeBase == 1)
			{
				double measure;
				if (pOldShape == 1)
					measure = (oldTime-pOldTime) * pOldBpm / 240;
				else
					measure = (oldTime-pOldTime) * (oldBpm+pOldBpm) / 480;

				if (pShape == 1)
					point.position = pTime + (240*measure) / pBpm;
				else
					point.position = pTime + (480*measure) / (pBpm + point.value);
			}

			// Update tempo point
			point.Append(newState);

			// Update data on previous point
			pTime = point.position;
			pBpm = point.value;
			pShape = point.shape;
			pOldTime = oldTime;
			pOldBpm = oldBpm;
			pOldShape = oldShape;
		}

		else
		{
			newState.Append(token);
			newState.Append("\n");
		}

		token = strtok(NULL, "\n");
	}

	// Update tempo chunk
	GetSetObjectState(envelope, newState.Get());
	FreeHeapPtr(envState);

	// Refresh tempo map
	double t, b; int n, d; bool s;
	GetTempoTimeSigMarker(NULL, 0, &t, NULL, NULL, &b, &n, &d, &s);
	SetTempoTimeSigMarker(NULL, 0, t, -1, -1, b, n, d, s);
	UpdateTimeline();

	// Restore preferences back to the previous state
	SetConfig("envclicksegmode", envClickSegMode);
}

static void UpdateTargetBpm (HWND hwnd, int doFirst, int doCursor, int doLast)
{
	char bpmAdj[128], bpm1Cur[128], bpm2Cur[128], bpm3Cur[128];
	double bpm1Tar, bpm2Tar, bpm3Tar;
	GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_1, bpm1Cur, 128);
	GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_2, bpm2Cur, 128);
	GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_3, bpm3Cur, 128);

	if (atof(bpm1Cur) == 0)
	{
		sprintf(bpm1Cur, "%d", 0);
		sprintf(bpm2Cur, "%d", 0);
		sprintf(bpm3Cur, "%d", 0);
	}
	else
	{
		// Calculate target
		if (IsDlgButtonChecked(hwnd, IDC_BR_ADJ_BPM_VAL_ENB))
		{
			GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_VAL, bpmAdj, 128);
			bpm1Tar = AltAtof(bpmAdj) + AltAtof(bpm1Cur);
			bpm2Tar = AltAtof(bpmAdj) + AltAtof(bpm2Cur);
			bpm3Tar = AltAtof(bpmAdj) + AltAtof(bpm3Cur);
		}
		else
		{
			GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_PERC, bpmAdj, 128);
			bpm1Tar = (1 + AltAtof(bpmAdj)/100) * AltAtof(bpm1Cur);
			bpm2Tar = (1 + AltAtof(bpmAdj)/100) * AltAtof(bpm2Cur);
			bpm3Tar = (1 + AltAtof(bpmAdj)/100) * AltAtof(bpm3Cur);
		}

		// Check values
		if (bpm1Tar < MIN_BPM) {bpm1Tar = MIN_BPM;} else if (bpm1Tar > MAX_BPM) {bpm1Tar = MAX_BPM;}
		if (bpm2Tar < MIN_BPM) {bpm2Tar = MIN_BPM;} else if (bpm2Tar > MAX_BPM) {bpm2Tar = MAX_BPM;}
		if (bpm3Tar < MIN_BPM) {bpm3Tar = MIN_BPM;} else if (bpm3Tar > MAX_BPM) {bpm3Tar = MAX_BPM;}

		sprintf(bpm1Cur, "%.6g", bpm1Tar);
		sprintf(bpm2Cur, "%.6g", bpm2Tar);
		sprintf(bpm3Cur, "%.6g", bpm3Tar);
	}

	// Update target edit boxes
	if (doFirst)
		SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_TAR_1, bpm1Cur);
	if (doCursor)
		SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_TAR_2, bpm2Cur);
	if (doLast)
		SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_TAR_3, bpm3Cur);
}

static void UpdateCurrentBpm (HWND hwnd, const vector<int>& selectedPoints)
{
	double bpmFirst = 0, bpmCursor = 0, bpmLast = 0;
	char eBpmFirst[128], eBpmCursor[128], eBpmLast[128], eBpmFirstChk[128], eBpmCursorChk[128], eBpmLastChk[128];

	// Get BPM info on selected points
	if (selectedPoints.size() != 0)
	{
		GetTempoTimeSigMarker(NULL, selectedPoints.front(), NULL, NULL, NULL, &bpmFirst, NULL, NULL, NULL);
		GetTempoTimeSigMarker(NULL, selectedPoints.back(), NULL, NULL, NULL, &bpmLast, NULL, NULL, NULL);
	}
	TimeMap_GetTimeSigAtTime(NULL, GetCursorPositionEx(NULL), NULL, NULL, &bpmCursor);
	sprintf(eBpmFirst, "%.6g", bpmFirst);
	sprintf(eBpmCursor, "%.6g", bpmCursor);
	sprintf(eBpmLast, "%.6g", bpmLast);

	// Get values from edit boxes
	GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_1, eBpmFirstChk, 128);
	GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_2, eBpmCursorChk, 128);
	GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_3, eBpmLastChk, 128);

	// Update edit boxes only if they've changed
	if (atof(eBpmFirst) != atof(eBpmFirstChk) || atof(eBpmCursor) != atof(eBpmCursorChk) || atof(eBpmLast) != atof(eBpmLastChk))
	{
		// Update current edit boxes
		SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_1, eBpmFirst);
		SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_2, eBpmCursor);
		SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_3, eBpmLast);

		// Update target edit boxes
		UpdateTargetBpm(hwnd, 1, 1, 1);
	}
}

static void UpdateSelectionFields (HWND hwnd)
{
	char eBpmStart[128], eBpmEnd[128], eNum[128], eDen[128];
	GetDlgItemText(hwnd, IDC_BR_SEL_BPM_START, eBpmStart, 128);
	GetDlgItemText(hwnd, IDC_BR_SEL_BPM_END, eBpmEnd, 128);
	GetDlgItemText(hwnd, IDC_BR_SEL_SIG_NUM, eNum, 128);
	GetDlgItemText(hwnd, IDC_BR_SEL_SIG_DEN, eDen, 128);

	double bpmStart = AltAtof(eBpmStart);
	double bpmEnd = AltAtof(eBpmEnd);
	int num = atoi(eNum); if (num < MIN_SIG){num = MIN_SIG;} else if (num > MAX_SIG){num = MAX_SIG;}
	int den = atoi(eDen); if (den < MIN_SIG){den = MIN_SIG;} else if (den > MAX_SIG){den = MAX_SIG;}

	// Update edit boxes with "atoied/atofed" values
	sprintf(eBpmStart, "%.19g", bpmStart);
	sprintf(eBpmEnd, "%.19g", bpmEnd);
	sprintf(eNum, "%d", num);
	sprintf(eDen, "%d", den);
	SetDlgItemText(hwnd, IDC_BR_SEL_BPM_START, eBpmStart);
	SetDlgItemText(hwnd, IDC_BR_SEL_BPM_END, eBpmEnd);
	SetDlgItemText(hwnd, IDC_BR_SEL_SIG_NUM, eNum);
	SetDlgItemText(hwnd, IDC_BR_SEL_SIG_DEN, eDen);
}

static void SelectTempoCase (HWND hwnd, int operationType, int unselectNth)
{
	/*
	operation type --> 0 to select
	               --> 1 to unselect, if unselectNth > 1 unselect every Nth selected marker
	               --> 2 to invert
	*/

	// Read values from the dialog
	int mode;
	if (IsDlgButtonChecked(hwnd, IDC_BR_SEL_ADD))
		mode = 4; // Add to existing selection
	else
		mode = 3; // Create new selection

	int bpm = IsDlgButtonChecked(hwnd, IDC_BR_SEL_BPM);
	int sig = IsDlgButtonChecked(hwnd, IDC_BR_SEL_SIG);
	int timeSel = (int)SendDlgItemMessage(hwnd,IDC_BR_SEL_TIME_RANGE,CB_GETCURSEL,0,0);
	int shape = (int)SendDlgItemMessage(hwnd,IDC_BR_SEL_SHAPE,CB_GETCURSEL,0,0);
	int type = (int)SendDlgItemMessage(hwnd,IDC_BR_SEL_TYPE,CB_GETCURSEL,0,0);

	char eBpmStart[128], eBpmEnd[128], eNum[128], eDen[128];
	GetDlgItemText(hwnd, IDC_BR_SEL_BPM_START, eBpmStart, 128);
	GetDlgItemText(hwnd, IDC_BR_SEL_BPM_END, eBpmEnd, 128);
	GetDlgItemText(hwnd, IDC_BR_SEL_SIG_NUM, eNum, 128);
	GetDlgItemText(hwnd, IDC_BR_SEL_SIG_DEN, eDen, 128);
	int num = atoi(eNum); if (num < MIN_SIG){num = MIN_SIG;} else if (num > MAX_SIG){num = MAX_SIG;}
	int den = atoi(eDen); if (den < MIN_SIG){den = MIN_SIG;} else if (den > MAX_SIG){den = MAX_SIG;}
	double bpmStart = AltAtof(eBpmStart);
	double bpmEnd = AltAtof(eBpmEnd);

	// Invert BPM values if needed
	if (bpmStart > bpmEnd)
	{
		double temp = bpmStart;
		bpmStart = bpmEnd;
		bpmEnd = temp;
	}

	// Select
	if (operationType == 0)
		SelectTempo (mode, 0, timeSel, bpm, bpmStart, bpmEnd, shape, sig, num, den, type);

	// Unselect
	if (operationType == 1)
	{
		if (unselectNth == 0)
			SelectTempo (5, 0, timeSel, bpm, bpmStart, bpmEnd, shape, sig, num, den, type);
		else
			SelectTempo (6, unselectNth, timeSel, bpm, bpmStart, bpmEnd, shape, sig, num, den, type);
	}

	// Invert
	if (operationType == 2)
		SelectTempo (7, 0, timeSel, bpm, bpmStart, bpmEnd, shape, sig, num, den, type);
}

static void AdjustTempoCase (HWND hwnd)
{
	char eBpmVal[128], eBpmPerc[128];
	GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_VAL, eBpmVal, 128);
	GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_PERC, eBpmPerc, 128);
	double bpmVal = AltAtof(eBpmVal);
	double bpmPerc = AltAtof(eBpmPerc);

	// Update edit boxes
	UpdateTargetBpm(hwnd, 1, 1, 1);
	sprintf(eBpmVal, "%.6g", bpmVal);
	sprintf(eBpmPerc, "%.6g", bpmPerc);
	SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_VAL, eBpmVal);
	SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_PERC, eBpmPerc);

	int mode; double bpm;
	if (IsDlgButtonChecked(hwnd, IDC_BR_ADJ_BPM_VAL_ENB))
	{
		mode = 0;       // Adjust by value
		bpm = bpmVal;
	}
	else
	{
		mode = 1;       // Adjust by percentage
		bpm = bpmPerc;
	}
	int shape = (int)SendDlgItemMessage(hwnd,IDC_BR_ADJ_SHAPE,CB_GETCURSEL,0,0);

	// Adjust markers
	if (bpm != 0 || shape != 0)
	{
		Undo_BeginBlock2(NULL);
		AdjustTempo (mode, bpm, shape);
		Undo_EndBlock2 (NULL, __LOCALIZE("Adjust selected tempo markers","sws_undo"), UNDO_STATE_ALL);

	}
}

static void SaveOptionsSelAdj (HWND hwnd)
{
	char eBpmStart[128], eBpmEnd[128], eNum[128], eDen[128];
	GetDlgItemText(hwnd, IDC_BR_SEL_BPM_START, eBpmStart, 128);
	GetDlgItemText(hwnd, IDC_BR_SEL_BPM_END, eBpmEnd, 128);
	GetDlgItemText(hwnd, IDC_BR_SEL_SIG_NUM, eNum, 128);
	GetDlgItemText(hwnd, IDC_BR_SEL_SIG_DEN, eDen, 128);
	double bpmStart = AltAtof(eBpmStart);
	double bpmEnd = AltAtof(eBpmEnd);
	int num = atoi(eNum);
	int den = atoi(eDen);
	int bpmEnb = IsDlgButtonChecked(hwnd, IDC_BR_SEL_BPM);
	int sigEnb = IsDlgButtonChecked(hwnd, IDC_BR_SEL_SIG);
	int timeSel = (int)SendDlgItemMessage(hwnd,IDC_BR_SEL_TIME_RANGE,CB_GETCURSEL,0,0);
	int shape = (int)SendDlgItemMessage(hwnd,IDC_BR_SEL_SHAPE,CB_GETCURSEL,0,0);
	int type = (int)SendDlgItemMessage(hwnd,IDC_BR_SEL_TYPE,CB_GETCURSEL,0,0);
	int selPref = IsDlgButtonChecked(hwnd, IDC_BR_SEL_ADD);
	int invertPref = IsDlgButtonChecked(hwnd, IDC_BR_SEL_INVERT_PREF);
	int adjustType = IsDlgButtonChecked(hwnd, IDC_BR_ADJ_BPM_VAL_ENB);
	int adjustShape = (int)SendDlgItemMessage(hwnd,IDC_BR_ADJ_SHAPE,CB_GETCURSEL,0,0);

	char tmp[256];
	_snprintf(tmp, sizeof(tmp), "%lf %lf %d %d %d %d %d %d %d %d %d %d %d", bpmStart, bpmEnd, num, den, bpmEnb, sigEnb, timeSel, shape, type, selPref, invertPref, adjustType, adjustShape);
	WritePrivateProfileString("SWS", SEL_ADJ_KEY, tmp, get_ini_file());
}

static void LoadOptionsSelAdj (double& bpmStart, double& bpmEnd, int& num, int& den, int& bpmEnb, int& sigEnb, int& timeSel, int& shape, int& type, int& selPref, int& invertPref, int& adjustType, int& adjustShape)
{
	char tmp[256];
	GetPrivateProfileString("SWS", SEL_ADJ_KEY, "120 150 4 4 1 0 0 0 0 0 0 0 0", tmp, 256, get_ini_file());
	sscanf(tmp, "%lf %lf %d %d %d %d %d %d %d %d %d %d %d", &bpmStart, &bpmEnd, &num, &den, &bpmEnb, &sigEnb, &timeSel, &shape, &type, &selPref, &invertPref, &adjustType, &adjustShape);

	// Restore defaults if needed
	if (num < MIN_SIG || num > MAX_SIG)
		num = MIN_SIG;
	if (den < MIN_SIG || den > MAX_SIG)
		den = MIN_SIG;
	if (bpmEnb != 0 && bpmEnb != 1)
		bpmEnb = 1;
	if (sigEnb != 0 && sigEnb != 1)
		sigEnb = 0;
	if (timeSel < 0 || timeSel > 2)
		timeSel = 0;
	if (shape < 0 || shape > 2)
		shape = 0;
	if (type < 0 || type > 2)
		type = 0;
	if (selPref != 0 && selPref != 1)
		selPref = 1;
	if (invertPref != 0 && invertPref != 1)
		invertPref = 0;
	if (adjustType != 0 && adjustType != 1)
		adjustType = 1;
	if (adjustShape < 0 || adjustShape > 3)
		adjustShape = 0;
}

WDL_DLGRET SelectAdjustTempoProc (HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (INT_PTR r = SNM_HookThemeColorsMessage(hwnd, uMsg, wParam, lParam))
		return r;

	switch(uMsg)
	{
		case WM_INITDIALOG:
		{
			// Drop downs
			WDL_UTF8_HookComboBox(GetDlgItem(hwnd, IDC_BR_SEL_TIME_RANGE));
			int x = (int)SendDlgItemMessage(hwnd, IDC_BR_SEL_TIME_RANGE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("All","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_SEL_TIME_RANGE, CB_SETITEMDATA, x, 0);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_SEL_TIME_RANGE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("Time selection","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_SEL_TIME_RANGE, CB_SETITEMDATA, x, 1);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_SEL_TIME_RANGE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("Ignore time selection","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_SEL_TIME_RANGE, CB_SETITEMDATA, x, 2);

			WDL_UTF8_HookComboBox(GetDlgItem(hwnd, IDC_BR_SEL_SHAPE));
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_SEL_SHAPE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("All","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_SEL_SHAPE, CB_SETITEMDATA, x, 0);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_SEL_SHAPE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("Square","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_SEL_SHAPE, CB_SETITEMDATA, x, 1);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_SEL_SHAPE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("Linear","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_SEL_SHAPE, CB_SETITEMDATA, x, 2);

			WDL_UTF8_HookComboBox(GetDlgItem(hwnd, IDC_BR_SEL_TYPE));
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_SEL_TYPE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("All","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_SEL_TYPE, CB_SETITEMDATA, x, 0);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_SEL_TYPE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("Tempo markers","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_SEL_TYPE, CB_SETITEMDATA, x, 1);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_SEL_TYPE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("Time signature markers","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_SEL_TYPE, CB_SETITEMDATA, x, 2);

			WDL_UTF8_HookComboBox(GetDlgItem(hwnd, IDC_BR_ADJ_SHAPE));
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_ADJ_SHAPE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("Preserve","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_ADJ_SHAPE, CB_SETITEMDATA, x, 0);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_ADJ_SHAPE,CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("Invert","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_ADJ_SHAPE, CB_SETITEMDATA, x, 1);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_ADJ_SHAPE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("Linear","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_ADJ_SHAPE, CB_SETITEMDATA, x, 2);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_ADJ_SHAPE, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("Square","sws_DLG_167"));
			SendDlgItemMessage(hwnd, IDC_BR_ADJ_SHAPE, CB_SETITEMDATA, x, 3);

			// Load options from .ini
			double bpmStart, bpmEnd;
			char eBpmStart[128], eBpmEnd[128], eNum[128], eDen[128];
			int num, den, bpmEnb, sigEnb, timeSel, shape, type, selPref, invertPref, adjustType, adjustShape;
			LoadOptionsSelAdj(bpmStart, bpmEnd, num, den, bpmEnb, sigEnb, timeSel, shape, type, selPref, invertPref, adjustType, adjustShape);
			sprintf(eBpmStart, "%.6g", bpmStart);
			sprintf(eBpmEnd, "%.6g", bpmEnd);
			sprintf(eNum, "%d", num);
			sprintf(eDen, "%d", den);

			// Find tempo at cursor
			char bpmCursor[128]; double effBpmCursor;
			TimeMap_GetTimeSigAtTime(NULL, GetCursorPositionEx(NULL), NULL, NULL, &effBpmCursor);
			sprintf(bpmCursor, "%.6g", effBpmCursor);

			// Set controls
			SetDlgItemText(hwnd, IDC_BR_SEL_BPM_START, eBpmStart);
			SetDlgItemText(hwnd, IDC_BR_SEL_BPM_END, eBpmEnd);
			SetDlgItemText(hwnd, IDC_BR_SEL_SIG_NUM, eNum);
			SetDlgItemText(hwnd, IDC_BR_SEL_SIG_DEN, eDen);
			SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_1, "0");
			SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_2, bpmCursor);
			SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_3, "0");
			SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_TAR_1, "0");
			SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_TAR_2, "0");
			SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_TAR_3, "0");
			SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_VAL, "0");
			SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_PERC, "0");
			CheckDlgButton(hwnd, IDC_BR_SEL_BPM, !!bpmEnb);
			CheckDlgButton(hwnd, IDC_BR_SEL_SIG, !!sigEnb);
			CheckDlgButton(hwnd, IDC_BR_SEL_ADD, !!selPref);
			CheckDlgButton(hwnd, IDC_BR_SEL_INVERT_PREF, !!invertPref);
			CheckDlgButton(hwnd, IDC_BR_ADJ_BPM_VAL_ENB, !!adjustType);
			CheckDlgButton(hwnd, IDC_BR_ADJ_BPM_PERC_ENB, !adjustType);
			EnableWindow(GetDlgItem(hwnd, IDC_BR_SEL_BPM_START), !!bpmEnb);
			EnableWindow(GetDlgItem(hwnd, IDC_BR_SEL_BPM_END), !!bpmEnb);
			EnableWindow(GetDlgItem(hwnd, IDC_BR_SEL_SIG_NUM), !!sigEnb);
			EnableWindow(GetDlgItem(hwnd, IDC_BR_SEL_SIG_DEN), !!sigEnb);
			EnableWindow(GetDlgItem(hwnd, IDC_BR_ADJ_BPM_VAL), !!adjustType);
			EnableWindow(GetDlgItem(hwnd, IDC_BR_ADJ_BPM_PERC), !adjustType);
			SendDlgItemMessage(hwnd, IDC_BR_SEL_SHAPE, CB_SETCURSEL, shape, 0);
			SendDlgItemMessage(hwnd, IDC_BR_SEL_TYPE, CB_SETCURSEL, type, 0);
			SendDlgItemMessage(hwnd, IDC_BR_SEL_TIME_RANGE, CB_SETCURSEL, timeSel, 0);
			SendDlgItemMessage(hwnd, IDC_BR_ADJ_SHAPE, CB_SETCURSEL, adjustShape, 0);
			#ifndef _WIN32
				RECT r;
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_SEL_BPM_STATIC), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_SEL_BPM_STATIC), HWND_BOTTOM, r.left-3, r.top+2, 0, 0, SWP_NOSIZE);
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_SEL_SIG_STATIC), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_SEL_SIG_STATIC), HWND_BOTTOM, r.left-3, r.top, 0, 0, SWP_NOSIZE);
			#endif

			RestoreWindowPos(hwnd, SEL_ADJ_WND, false);
		}
		break;

		case WM_COMMAND:
		{
			switch(LOWORD(wParam))
			{
				////// SELECT MARKERS //////

				// Check boxes
				case IDC_BR_SEL_BPM:
				{
					int x = IsDlgButtonChecked(hwnd, IDC_BR_SEL_BPM);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_SEL_BPM_START), !!x);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_SEL_BPM_END), !!x);
				}
				break;

				case IDC_BR_SEL_SIG:
				{
					int x = IsDlgButtonChecked(hwnd, IDC_BR_SEL_SIG);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_SEL_SIG_NUM), !!x);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_SEL_SIG_DEN), !!x);
				}
				break;

				// Control buttons
				case IDC_BR_SEL_SELECT:
				{
					Undo_BeginBlock2(NULL);
					UpdateSelectionFields(hwnd);
					SelectTempoCase(hwnd, 0, 0);
					Undo_EndBlock2(NULL, __LOCALIZE("Select tempo markers","sws_undo"), UNDO_STATE_ALL);
				}
				break;

				case IDC_BR_SEL_UNSELECT:
				{
					Undo_BeginBlock2(NULL);
					UpdateSelectionFields(hwnd);
					SelectTempoCase(hwnd, 1, 0);
					Undo_EndBlock2(NULL, __LOCALIZE("Unselect tempo markers","sws_undo"), UNDO_STATE_ALL);
				}
				break;

				case IDC_BR_SEL_INVERT:
				{
					Undo_BeginBlock2(NULL);
					UpdateSelectionFields(hwnd);
					if (IsDlgButtonChecked(hwnd, IDC_BR_SEL_INVERT_PREF) == 0)
						SelectTempo(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
					else
						SelectTempoCase(hwnd, 2, 0);
					Undo_EndBlock2(NULL, __LOCALIZE("Invert selection of tempo markers","sws_undo"), UNDO_STATE_ALL);
				}
				break;

				case IDC_BR_SEL_CLEAR:
				{
					Undo_BeginBlock2(NULL);
					UpdateSelectionFields(hwnd);
					SelectTempo(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
					Undo_EndBlock2(NULL, __LOCALIZE("Unselect tempo markers","sws_undo"), UNDO_STATE_ALL);
				}
				break;

				case IDC_BR_SEL_UNSELECT_NTH:
				{
					UnselectNthDialog(true, hwnd); // show child dialog and pass parent's handle to it
				}
				break;


				////// ADJUST MARKERS //////

				// Radio buttons
				case IDC_BR_ADJ_BPM_VAL_ENB:
				{
					int enb = IsDlgButtonChecked(hwnd, IDC_BR_ADJ_BPM_VAL_ENB);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_ADJ_BPM_VAL), !!enb);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_ADJ_BPM_PERC), !enb);
					UpdateTargetBpm(hwnd, 1, 1, 1);
				}
				break;

				case IDC_BR_ADJ_BPM_PERC_ENB:
				{
					int enb = IsDlgButtonChecked(hwnd, IDC_BR_ADJ_BPM_PERC_ENB);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_ADJ_BPM_PERC), !!enb);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_ADJ_BPM_VAL), !enb);
					UpdateTargetBpm(hwnd, 1, 1, 1);
				}
				break;

				// Edit boxes
				case IDC_BR_ADJ_BPM_VAL:
				{
					if (HIWORD(wParam) == EN_CHANGE && GetFocus() == GetDlgItem(hwnd, IDC_BR_ADJ_BPM_VAL))
						UpdateTargetBpm(hwnd, 1, 1, 1);
				}
				break;

				case IDC_BR_ADJ_BPM_PERC:
				{
					if (HIWORD(wParam) == EN_CHANGE && GetFocus() == GetDlgItem(hwnd, IDC_BR_ADJ_BPM_PERC))
						UpdateTargetBpm(hwnd, 1, 1, 1);
				}
				break;

				case IDC_BR_ADJ_BPM_TAR_1:
				{
					if (HIWORD(wParam) == EN_CHANGE && GetFocus() == GetDlgItem(hwnd, IDC_BR_ADJ_BPM_TAR_1))
					{
						char bpmTar[128], bpmCur[128];
						double bpmVal, bpmPerc;
						GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_TAR_1, bpmTar, 128);
						GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_1, bpmCur, 128);

						bpmVal = AltAtof(bpmTar) - atof(bpmCur);
						if (atof(bpmCur) != 0)
							bpmPerc = bpmVal / atof(bpmCur) * 100;
						else
							bpmPerc = 0;

						sprintf(bpmTar, "%.6g", bpmVal);
						sprintf(bpmCur, "%.6g", bpmPerc);
						SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_VAL, bpmTar);
						SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_PERC, bpmCur);
						UpdateTargetBpm(hwnd, 0, 1, 1);
					}
				}
				break;

				case IDC_BR_ADJ_BPM_TAR_2:
				{
					if (HIWORD(wParam) == EN_CHANGE && GetFocus() == GetDlgItem(hwnd, IDC_BR_ADJ_BPM_TAR_2))
					{
						char bpmTar[128], bpmCur[128];
						double bpmVal, bpmPerc;
						GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_TAR_2, bpmTar, 128);
						GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_2, bpmCur, 128);

						bpmVal = AltAtof(bpmTar) - atof(bpmCur);
						if (atof(bpmCur) != 0)
							bpmPerc = bpmVal / atof(bpmCur) * 100;
						else
							bpmPerc = 0;

						sprintf(bpmTar, "%.6g", bpmVal);
						sprintf(bpmCur, "%.6g", bpmPerc);
						SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_VAL, bpmTar);
						SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_PERC, bpmCur);
						UpdateTargetBpm(hwnd, 1, 0, 1);
					}
				}
				break;

				case IDC_BR_ADJ_BPM_TAR_3:
				{
					if (HIWORD(wParam) == EN_CHANGE && GetFocus() == GetDlgItem(hwnd, IDC_BR_ADJ_BPM_TAR_3))
					{
						char bpmTar[128], bpmCur[128];
						double bpmVal, bpmPerc;
						GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_TAR_3, bpmTar, 128);
						GetDlgItemText(hwnd, IDC_BR_ADJ_BPM_CUR_3, bpmCur, 128);

						bpmVal = AltAtof(bpmTar) - atof(bpmCur);
						if (atof(bpmCur) != 0)
							bpmPerc = bpmVal / atof(bpmCur) * 100;
						else
							bpmPerc = 0;

						sprintf(bpmTar, "%.6g", bpmVal);
						sprintf(bpmCur, "%.6g", bpmPerc);
						SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_VAL, bpmTar);
						SetDlgItemText(hwnd, IDC_BR_ADJ_BPM_PERC, bpmCur);
						UpdateTargetBpm(hwnd, 1, 1, 0);
					}
				}
				break;

				// Control buttons
				case IDC_BR_ADJ_APPLY:
				{
					AdjustTempoCase(hwnd);
				}
				break;

				case IDCANCEL:
				{
					SelectAdjustTempoDialog(NULL);
				}
				break;
			}
		}
		break;

		case WM_TIMER:
		{
			// Get number of selected points and set Windows caption
			vector<int> selectedPoints = GetSelPoints(GetTempoEnv());
			char pointCount[512];
			_snprintf(pointCount, sizeof(pointCount), __LOCALIZE_VERFMT("SWS/BR - Select and adjust tempo markers (%d of %d points selected)","sws_DLG_167") , selectedPoints.size(), CountTempoTimeSigMarkers(NULL) );
			SetWindowText(hwnd,pointCount);

			// Update current and target edit boxes
			UpdateCurrentBpm(hwnd, selectedPoints);
		}
		break;

		case WM_DESTROY:
		{
			SaveWindowPos(hwnd, SEL_ADJ_WND);
			SaveOptionsSelAdj(hwnd);
		}
		break;
	}
	return 0;
}

/******************************************************************************
* Unselect Nth selected tempo markers                                         *
******************************************************************************/
static void SaveOptionsUnselectNth (HWND hwnd)
{
	int Nth = (int)SendDlgItemMessage(hwnd,IDC_BR_UNSEL_NTH_TEMPO,CB_GETCURSEL,0,0);
	int criteria = IsDlgButtonChecked(hwnd, IDC_BR_UNSEL_CRITERIA);

	char tmp[256];
	_snprintf(tmp, sizeof(tmp), "%d %d", Nth, criteria);
	WritePrivateProfileString("SWS", UNSEL_KEY, tmp, get_ini_file());
}

static void LoadOptionsUnselectNth (int& Nth, int& criteria)
{
	char tmp[256];
	GetPrivateProfileString("SWS", UNSEL_KEY, "0 0", tmp, 256, get_ini_file());
	sscanf(tmp, "%d %d ", &Nth, &criteria);

	// Restore defaults if needed
	if (Nth < 0 && Nth > 14)
		Nth = 0;
	if (criteria != 0 && criteria != 1)
		criteria = 1;
}

WDL_DLGRET UnselectNthProc (HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (INT_PTR r = SNM_HookThemeColorsMessage(hwnd, uMsg, wParam, lParam))
		return r;

	switch(uMsg)
	{
		case WM_INITDIALOG:
		{
			// Drop down menu
			WDL_UTF8_HookComboBox(GetDlgItem(hwnd, IDC_BR_UNSEL_NTH_TEMPO));

			int x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("2nd","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 0);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("3rd","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 1);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("4th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 2);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("5th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 3);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("6th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 4);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("7th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 5);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("8th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 6);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("9th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 7);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("10th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 8);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("11th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 9);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("12th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 10);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("13th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 11);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("14th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 12);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("15th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 13);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("16th","sws_DLG_168"));
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETITEMDATA, x, 14);

			// Load options from .ini
			int Nth, criteria;
			LoadOptionsUnselectNth (Nth, criteria);

			// Set controls
			SendDlgItemMessage(hwnd, IDC_BR_UNSEL_NTH_TEMPO, CB_SETCURSEL, Nth, 0);
			CheckDlgButton(hwnd, IDC_BR_UNSEL_CRITERIA, !!criteria);

			RestoreWindowPos(hwnd, UNSEL_WND, false);
		}
		break;

		case WM_COMMAND:
		{
			switch(LOWORD(wParam))
			{
				case IDOK:
				{
					int Nth = (int)SendDlgItemMessage(hwnd,IDC_BR_UNSEL_NTH_TEMPO,CB_GETCURSEL,0,0) + 2;
					Undo_BeginBlock2(NULL);
					if (IsDlgButtonChecked(hwnd, IDC_BR_UNSEL_CRITERIA))
						SelectTempoCase (GetParent(hwnd), 1, Nth);
					else
						SelectTempo (2, Nth, 0, 0, 0, 0, 0, 0, 0, 0, 0);
					Undo_EndBlock2 (NULL, __LOCALIZE("Unselect tempo markers","sws_undo"), UNDO_STATE_ALL);
				}
				break;

				case IDCANCEL:
				{
					UnselectNthDialog (false, NULL);
				}
				break;
			}
		}
		break;

		case WM_DESTROY:
		{
			SaveWindowPos(hwnd, UNSEL_WND);
			SaveOptionsUnselectNth (hwnd);
		}
		break;
	}
	return 0;
}

void UnselectNthDialog (bool show, HWND parentHandle)
{
	static HWND hwnd = CreateDialog (g_hInst, MAKEINTRESOURCE(IDD_BR_UNSELECT_TEMPO), parentHandle, UnselectNthProc);
	static bool visible = false;

	if (show)
	{
		// this lets user toggle the dialog from the parent dialog
		if (!visible)
		{
			ShowWindow(hwnd, SW_SHOW);
			SetFocus(hwnd);
			visible = true;
		}
		else
		{
			ShowWindow(hwnd, SW_HIDE);
			visible = false;
		}
	}
	else
	{
		ShowWindow(hwnd, SW_HIDE);
		visible = false;
	}
}

/******************************************************************************
* Randomize selected tempo markers                                            *
******************************************************************************/
static void SetRandomTempo (HWND hwnd, BR_Envelope* oldTempo, double min, double max, int unit, double minLimit, double maxLimit, int unitLimit, int limit)
{
	BR_Envelope tempoMap = *oldTempo;
	int timeBase; GetConfig("tempoenvtimelock", timeBase); // TEMPO MARKERS timebase (not project timebase)

	// Hold value for previous point
	double t0, b0;
	int s0;
	tempoMap.GetPoint(0, &t0, &b0, &s0, NULL);
	double t0_old = t0;
	double b0_old = b0;

	// Go through all tempo points and randomize tempo
	for (int i = 0; i < tempoMap.Count(); ++i)
	{
		// Get current point
		double t1, b1;
		int s1;
		tempoMap.GetPoint(i, &t1, &b1, &s1, NULL);

		// If point is selected calculate it's new BPM
		double newBpm = b1;
		if (tempoMap.GetSelection(i))
		{
			double random = (double)(rand() % 101) / 100;

			// Calculate new bpm
			if (unit == 0)        // Value
				newBpm = (b1 + min) + ((max-min) * random);
			else                  // Percentage
				newBpm = (b1 * (100 + min + random*(max-min)))/100;

			// Check against limits
			if (limit)
			{
				// Value
				if (unitLimit == 0)
				{
					if (newBpm < minLimit)
						newBpm = minLimit;
					else if (newBpm > maxLimit)
						newBpm = maxLimit;
				}
				// Percentage
				else
				{
					if (newBpm < b1 * (1 + minLimit/100))
						newBpm = b1 * (1 + minLimit/100);
					else if (newBpm > b1 * (1 + maxLimit/100))
						newBpm = b1 * (1 + maxLimit/100);
				}
			}

			// Check if BPM is legal
			if (newBpm < MIN_BPM)
				newBpm = MIN_BPM;
			else if (newBpm > MAX_BPM)
				newBpm = MAX_BPM;
		}

		// Get new position but only if timebase is beats
		double newTime = t1;
		if (timeBase == 1)
		{
			if (s0 == SQUARE)
				newTime = t0 + ((t1 - t0_old) * b0_old) / b0;
			else
				newTime = t0 + ((t1 - t0_old) * (b0_old + b1)) / (b0 + newBpm);
		}

		// Update data on previous point
		t0 = newTime;
		b0 = newBpm;
		s0 = s1;
		t0_old = t1;
		b0_old = b1;

		tempoMap.SetPoint(i, &newTime, &newBpm, NULL, NULL);
	}

	// Update tempo
	tempoMap.Commit(true);
}

static void SaveOptionsRandomizeTempo (HWND hwnd)
{
	char eMin[128], eMax[128], eMinLimit[128], eMaxLimit[128];
	GetDlgItemText(hwnd, IDC_BR_RAND_MIN, eMin, 128);
	GetDlgItemText(hwnd, IDC_BR_RAND_MAX, eMax, 128);
	GetDlgItemText(hwnd, IDC_BR_RAND_LIMIT_MIN, eMinLimit, 128);
	GetDlgItemText(hwnd, IDC_BR_RAND_LIMIT_MAX, eMaxLimit, 128);

	double min = AltAtof(eMin);
	double max = AltAtof(eMax);
	double minLimit = AltAtof(eMinLimit);
	double maxLimit = AltAtof(eMaxLimit);
	int unit = (int)SendDlgItemMessage(hwnd,IDC_BR_RAND_UNIT,CB_GETCURSEL,0,0);
	int unitLimit = (int)SendDlgItemMessage(hwnd,IDC_BR_RAND_LIMIT_UNIT,CB_GETCURSEL,0,0);
	int limit = IsDlgButtonChecked(hwnd, IDC_BR_RAND_LIMIT);

	char tmp[256];
	_snprintf(tmp, sizeof(tmp), "%lf %lf %d %lf %lf %d %d", min, max, unit, minLimit, maxLimit, unitLimit, limit);
	WritePrivateProfileString("SWS", RAND_KEY, tmp, get_ini_file());
}

static void LoadOptionsRandomizeTempo (double& min, double& max, int& unit, double& minLimit, double& maxLimit, int& unitLimit, int& limit)
{
	char tmp[256];
	GetPrivateProfileString("SWS", RAND_KEY, "-1 1 0 40 260 0 0", tmp, 256, get_ini_file());
	sscanf(tmp, "%lf %lf %d %lf %lf %d %d", &min, &max, &unit, &minLimit, &maxLimit, &unitLimit, &limit);

	// Restore defaults if needed
	if (unit != 0 && unit != 1)
		unit = 0;
	if (unitLimit != 0 && unitLimit != 1)
		unitLimit = 0;
	if (limit != 0 && limit != 1)
		limit = 0;
}

WDL_DLGRET RandomizeTempoProc (HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (INT_PTR r = SNM_HookThemeColorsMessage(hwnd, uMsg, wParam, lParam))
		return r;

	static BR_Envelope* oldTempo = NULL;
	static int envClickSegMode;
	static int undoMask;

	switch(uMsg)
	{
		case WM_INITDIALOG:
		{
			// Save and set preferences
			GetConfig("envclicksegmode", envClickSegMode);
			GetConfig("undomask", undoMask);
			SetConfig("envclicksegmode", ClearBit(envClickSegMode, 6));  // prevent reselection of points in time selection
			SetConfig("undomask", ClearBit(undoMask, 3));                // turn off undo for edit cursor

			// Get current tempo
			oldTempo = new (nothrow) BR_Envelope(GetTempoEnv());

			// Drop lists
			WDL_UTF8_HookComboBox(GetDlgItem(hwnd, IDC_BR_RAND_UNIT));
			int x = (int)SendDlgItemMessage(hwnd, IDC_BR_RAND_UNIT, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("BPM","sws_DLG_171"));
			SendDlgItemMessage(hwnd, IDC_BR_RAND_UNIT, CB_SETITEMDATA, x, 0);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_RAND_UNIT, CB_ADDSTRING, 0, (LPARAM)"%");
			SendDlgItemMessage(hwnd, IDC_BR_RAND_UNIT, CB_SETITEMDATA, x, 1);
			WDL_UTF8_HookComboBox(GetDlgItem(hwnd, IDC_BR_RAND_LIMIT_UNIT));
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_RAND_LIMIT_UNIT, CB_ADDSTRING, 0, (LPARAM)__LOCALIZE("BPM","sws_DLG_171"));
			SendDlgItemMessage(hwnd, IDC_BR_RAND_LIMIT_UNIT, CB_SETITEMDATA, x, 0);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_RAND_LIMIT_UNIT, CB_ADDSTRING, 0, (LPARAM)"%");
			SendDlgItemMessage(hwnd, IDC_BR_RAND_LIMIT_UNIT, CB_SETITEMDATA, x, 1);

			// Load options from .ini
			double min, max, minLimit, maxLimit;
			char eMin[128], eMax[128], eMinLimit[128], eMaxLimit[128];
			int unit, unitLimit, limit;
			LoadOptionsRandomizeTempo (min, max, unit, minLimit, maxLimit, unitLimit, limit);
			sprintf(eMin, "%.19g", min);
			sprintf(eMax, "%.19g", max);
			sprintf(eMinLimit, "%.19g", minLimit);
			sprintf(eMaxLimit, "%.19g", maxLimit);

			// Set controls
			SetDlgItemText(hwnd, IDC_BR_RAND_MIN, eMin);
			SetDlgItemText(hwnd, IDC_BR_RAND_MAX, eMax);
			SetDlgItemText(hwnd, IDC_BR_RAND_LIMIT_MIN, eMinLimit);
			SetDlgItemText(hwnd, IDC_BR_RAND_LIMIT_MAX, eMaxLimit);
			CheckDlgButton(hwnd, IDC_BR_RAND_LIMIT, !!limit);
			EnableWindow(GetDlgItem(hwnd, IDC_BR_RAND_LIMIT_MIN), !!limit);
			EnableWindow(GetDlgItem(hwnd, IDC_BR_RAND_LIMIT_MAX), !!limit);
			SendDlgItemMessage(hwnd, IDC_BR_RAND_UNIT, CB_SETCURSEL, unit, 0);
			SendDlgItemMessage(hwnd, IDC_BR_RAND_LIMIT_UNIT, CB_SETCURSEL, unitLimit, 0);
			#ifndef _WIN32
				RECT r;
				int c = 2;
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_RAND_UNIT), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_RAND_UNIT), HWND_TOP, r.left, r.top+c, 0, 0, SWP_NOSIZE);
				GetWindowRect(GetDlgItem(hwnd, IDC_BR_RAND_LIMIT_UNIT), &r); ScreenToClient(hwnd, (LPPOINT)&r);
				SetWindowPos(GetDlgItem(hwnd, IDC_BR_RAND_LIMIT_UNIT), HWND_TOP, r.left, r.top+c, 0, 0, SWP_NOSIZE);
			#endif

			// Set new random tempo and preview it
			SetRandomTempo(hwnd, oldTempo, min, max, unit, minLimit, maxLimit, unitLimit, limit);
			RestoreWindowPos(hwnd, RAND_WND, false);
		}
		break;

		case WM_COMMAND:
		{
			switch(LOWORD(wParam))
			{
				case IDC_BR_RAND_LIMIT:
				{
					int limit = IsDlgButtonChecked(hwnd, IDC_BR_RAND_LIMIT);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_RAND_LIMIT_MIN), !!limit);
					EnableWindow(GetDlgItem(hwnd, IDC_BR_RAND_LIMIT_MAX), !!limit);
				}
				break;

				case IDC_BR_RAND_SEED:
				{
					// Get info from the dialog
					char eMin[128], eMax[128], eMinLimit[128], eMaxLimit[128];
					GetDlgItemText(hwnd, IDC_BR_RAND_MIN, eMin, 128);
					GetDlgItemText(hwnd, IDC_BR_RAND_MAX, eMax, 128);
					GetDlgItemText(hwnd, IDC_BR_RAND_LIMIT_MIN, eMinLimit, 128);
					GetDlgItemText(hwnd, IDC_BR_RAND_LIMIT_MAX, eMaxLimit, 128);

					double min = AltAtof(eMin);
					double max = AltAtof(eMax);
					double minLimit = AltAtof(eMinLimit);
					double maxLimit = AltAtof(eMaxLimit);
					int limit = IsDlgButtonChecked(hwnd, IDC_BR_RAND_LIMIT);
					int unit = (int)SendDlgItemMessage(hwnd,IDC_BR_RAND_UNIT,CB_GETCURSEL,0,0);
					int unitLimit = (int)SendDlgItemMessage(hwnd,IDC_BR_RAND_LIMIT_UNIT,CB_GETCURSEL,0,0);

					// Update edit boxes with "atofed" values
					sprintf(eMin, "%.19g", min);
					sprintf(eMax, "%.19g", max);
					sprintf(eMinLimit, "%.19g", minLimit);
					sprintf(eMaxLimit, "%.19g", maxLimit);
					SetDlgItemText(hwnd, IDC_BR_RAND_MIN, eMin);
					SetDlgItemText(hwnd, IDC_BR_RAND_MAX, eMax);
					SetDlgItemText(hwnd, IDC_BR_RAND_LIMIT_MIN, eMinLimit);
					SetDlgItemText(hwnd, IDC_BR_RAND_LIMIT_MAX, eMaxLimit);

					// Set new random tempo and preview it
					SetRandomTempo(hwnd, oldTempo, min, max, unit, minLimit, maxLimit, unitLimit, limit);
				}
				break;

				case IDOK:
				{
					SetConfig("undomask", undoMask); // treat undo behavior of edit cursor per user preference
					Undo_OnStateChangeEx2(NULL, __LOCALIZE("Randomize selected tempo markers", "sws_undo"), UNDO_STATE_ALL, -1);
					EndDialog(hwnd, 0);
				}
				break;

				case IDCANCEL:
				{
					// Set old tempo
					oldTempo->Commit(true);
					EndDialog(hwnd, 0);
				}
				break;
			}
		}
		break;

		case WM_DESTROY:
		{
			delete oldTempo;
			oldTempo = NULL;
			SaveOptionsRandomizeTempo(hwnd);
			SaveWindowPos(hwnd, RAND_WND);

			// Restore preferences
			SetConfig("envclicksegmode", envClickSegMode);
			SetConfig("undomask", undoMask);
		}
		break;
	}
	return 0;
}

/******************************************************************************
* Set tempo marker shape (options)                                            *
******************************************************************************/
static void SaveOptionsTempoShape (HWND hwnd)
{
	int split = IsDlgButtonChecked(hwnd, IDC_BR_SHAPE_SPLIT);
	char splitRatio[128]; GetDlgItemText(hwnd, IDC_BR_SHAPE_SPLIT_RATIO, splitRatio, 128);

	char tmp[256];
	_snprintf(tmp, sizeof(tmp), "%d %s", split, splitRatio);
	WritePrivateProfileString("SWS", SHAPE_KEY, tmp, get_ini_file());
}

static void LoadOptionsTempoShape (int& split, char* splitRatio)
{
	char tmp[256];
	GetPrivateProfileString("SWS", SHAPE_KEY, "0 4/8", tmp, 256, get_ini_file());
	sscanf(tmp, "%d %s", &split, splitRatio);

	// Restore defaults if needed
	double convertedRatio;
	IsFraction (splitRatio, convertedRatio);
	if (split != 0 && split != 1)
		split = 0;
	if (convertedRatio <= 0 || convertedRatio >= 1)
		strcpy(splitRatio, "0");
}

static void SetTempoShapeOptions (int& split, char* splitRatio)
{
	if (split == 0)
		g_tempoShapeSplitMiddle = 0;
	else
		g_tempoShapeSplitMiddle = 1;

	double convertedRatio; IsFraction (splitRatio, convertedRatio);
	if (convertedRatio <= 0 || convertedRatio >= 1)
		g_tempoShapeSplitRatio = 0;
	else
		g_tempoShapeSplitRatio = convertedRatio;
};

bool GetTempoShapeOptions (double* splitRatio)
{
	if (g_tempoShapeSplitMiddle == -1)
	{
		int split; char splitRatio[128];
		LoadOptionsTempoShape(split, splitRatio);
		SetTempoShapeOptions (split, splitRatio);
	}

	WritePtr(splitRatio, g_tempoShapeSplitRatio);
	if (g_tempoShapeSplitMiddle == 1 && g_tempoShapeSplitRatio != 0)
		return true;
	else
		return false;
}

WDL_DLGRET TempoShapeOptionsProc (HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (INT_PTR r = SNM_HookThemeColorsMessage(hwnd, uMsg, wParam, lParam))
		return r;

	switch(uMsg)
	{
		case WM_INITDIALOG:
		{
			// Dropdown (split ratio)
			WDL_UTF8_HookComboBox(GetDlgItem(hwnd, IDC_BR_SHAPE_SPLIT_RATIO));
			int x = (int)SendDlgItemMessage(hwnd, IDC_BR_SHAPE_SPLIT_RATIO, CB_ADDSTRING, 0, (LPARAM)"1/2");
			SendDlgItemMessage(hwnd, IDC_BR_SHAPE_SPLIT_RATIO, CB_SETITEMDATA, x, 0);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_SHAPE_SPLIT_RATIO, CB_ADDSTRING, 0, (LPARAM)"1/3");
			SendDlgItemMessage(hwnd, IDC_BR_SHAPE_SPLIT_RATIO, CB_SETITEMDATA, x, 1);
			x = (int)SendDlgItemMessage(hwnd, IDC_BR_SHAPE_SPLIT_RATIO, CB_ADDSTRING, 0, (LPARAM)"1/4");
			SendDlgItemMessage(hwnd, IDC_BR_SHAPE_SPLIT_RATIO, CB_SETITEMDATA, x, 2);

			// Load options from .ini file
			int split;
			char splitRatio[128];
			LoadOptionsTempoShape (split, splitRatio);

			// Set controls and global variables
			SetDlgItemText(hwnd, IDC_BR_SHAPE_SPLIT_RATIO, splitRatio);
			CheckDlgButton(hwnd, IDC_BR_SHAPE_SPLIT, !!split);
			EnableWindow(GetDlgItem(hwnd, IDC_BR_SHAPE_SPLIT_RATIO), !!split);
			SetTempoShapeOptions (split, splitRatio);

			RestoreWindowPos(hwnd, SHAPE_WND, false);
		}
		break;

		case WM_COMMAND:
		{
			switch(LOWORD(wParam))
			{
				case IDC_BR_SHAPE_SPLIT:
				{
					// Read dialog values
					int split = IsDlgButtonChecked(hwnd, IDC_BR_SHAPE_SPLIT);
					char splitRatio[128]; GetDlgItemText(hwnd, IDC_BR_SHAPE_SPLIT_RATIO, splitRatio, 128);

					// Check split ratio
					double convertedRatio; IsFraction(splitRatio, convertedRatio);
					if (convertedRatio <= 0 || convertedRatio >= 1)
						strcpy(splitRatio, "0");

					// Set global variable and update split ratio edit box
					SetTempoShapeOptions (split, splitRatio);
					SetDlgItemText(hwnd, IDC_BR_SHAPE_SPLIT_RATIO, splitRatio);

					// Enable/disable check boxes
					EnableWindow(GetDlgItem(hwnd, IDC_BR_SHAPE_SPLIT_RATIO), !!split);
				}
				break;

				case IDC_BR_SHAPE_SPLIT_RATIO:
				{
					// Read dialog values
					int split = IsDlgButtonChecked(hwnd, IDC_BR_SHAPE_SPLIT);
					char splitRatio[128]; GetDlgItemText(hwnd, IDC_BR_SHAPE_SPLIT_RATIO, splitRatio, 128);

					// Check split ratio
					double convertedRatio; IsFraction(splitRatio, convertedRatio);
					if (convertedRatio <= 0 || convertedRatio >= 1)
						strcpy(splitRatio, "0");

					// Set global variable
					SetTempoShapeOptions(split, splitRatio);
				}
				break;

				case IDCANCEL:
				{
					TempoShapeOptionsDialog(NULL);
				}
				break;
			}
		}
		break;

		case WM_DESTROY:
		{
			SaveWindowPos(hwnd, SHAPE_WND);
			SaveOptionsTempoShape(hwnd);
		}
		break;
	}
	return 0;
}