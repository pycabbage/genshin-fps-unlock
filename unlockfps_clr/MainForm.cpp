#include "Unmanaged.h"
#include "MainForm.h"
#include "SettingsForm.h"
#include "AboutForm.h"
#include "Managed.h"
#include "resource.h"
#include <unordered_map>

#define MUTEX_NAME "GenshinFPSUnlocker"

using namespace System;
using namespace Reflection;
using namespace Windows::Forms;
using namespace Text::Json;
using namespace IO;
using namespace Threading;
using namespace Diagnostics;

static const std::unordered_map<std::string, LPCSTR> dependencies = {
	{"Microsoft.Bcl.AsyncInterfaces.dll", MAKEINTRESOURCEA(IDR_DLL1)},
	{"System.Buffers.dll", MAKEINTRESOURCEA(IDR_DLL2)},
	{"System.Memory.dll", MAKEINTRESOURCEA(IDR_DLL3)},
	{"System.Numerics.Vectors.dll", MAKEINTRESOURCEA(IDR_DLL4)},
	{"System.Runtime.CompilerServices.Unsafe.dll", MAKEINTRESOURCEA(IDR_DLL5)},
	{"System.Text.Encodings.Web.dll", MAKEINTRESOURCEA(IDR_DLL6)},
	{"System.Text.Json.dll", MAKEINTRESOURCEA(IDR_DLL7)},
	{"System.Threading.Tasks.Extensions.dll", MAKEINTRESOURCEA(IDR_DLL8)}
};

void Run()
{
	Application::SetCompatibleTextRenderingDefault(false);
	Application::EnableVisualStyles();

	unlockfpsclr::MainForm mainForm;
	Application::Run(%mainForm);
}


Assembly^ OnAssemblyResolve(Object^ sender, ResolveEventArgs^ args)
{
	/*
		Resolve dependencies through cpp resource
		the goal is to have a single file application
		kinda ugly but it works idc

		an easier way is to use vmprotect to bundle
		dependency assemblies, but ppl be skeptical about
		binaries processed through vmp, so I didn't use that

		for some reason vmprotect adds significant amount of il code
		even if there aren't any protection enabled

		ILMerge and ILRepack doesn't want to work with c++/cli
		if anyone knows an easier way to bundle assemblies, lmk
	*/

	Assembly^ assembly = nullptr;
	auto assemblyName = gcnew AssemblyName(args->Name);
	auto nativeString = static_cast<LPSTR>(static_cast<PVOID>(
		Marshal::StringToHGlobalAnsi(assemblyName->Name + ".dll")));

	auto resId = dependencies.find(nativeString);
	if (resId != dependencies.end())
	{
		auto rsrc = FindResourceA(nullptr, resId->second, "DLL");
		auto size = SizeofResource(nullptr, rsrc);
		auto rsrcData = LoadResource(nullptr, rsrc);
		auto pData = LockResource(rsrcData);
		if (pData)
		{
			auto rawBytes = gcnew array<BYTE>(size);
			Marshal::Copy(static_cast<IntPtr>(pData), rawBytes, 0, size);
			assembly = Assembly::Load(rawBytes);
		}
	}

	Marshal::FreeHGlobal(static_cast<IntPtr>(nativeString));

	return assembly;
}

int main(array<String^>^ args)
{
	HANDLE hMutex = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, MUTEX_NAME);
	if (hMutex) return 0;

	hMutex = CreateMutexA(nullptr, FALSE, MUTEX_NAME);

	AppDomain::CurrentDomain->AssemblyResolve += gcnew ResolveEventHandler(&OnAssemblyResolve);

	// Check to see if the unlocker is placed with the game
	if (File::Exists("UnityPlayer.dll") && (File::Exists("GenshinImpact.exe") || File::Exists("YuanShen.exe")))
	{
		MessageBox::Show("Do not place unlocker in the same folder as the game.", "Genshin Impact FPS Unlocker",
		                 MessageBoxButtons::OK, MessageBoxIcon::Warning);
		goto Exit;
	}

	auto thread = gcnew Thread(gcnew ThreadStart(Run));
	thread->SetApartmentState(ApartmentState::STA); // Single-Threaded Apartment required for file browse
	thread->Start();
	thread->Join();

Exit:
	ReleaseMutex(hMutex);
	return 0;
}

namespace unlockfpsclr
{
	Void MainForm::btnStartGame_Click(Object^ sender, EventArgs^ e)
	{
		// Exit the unlocker if create process was successful
		if (Managed::StartGame(settings))
		{
			this->WindowState = FormWindowState::Minimized;
			settings->Save();
		}
	}

	Void MainForm::settingsMenuItem_Click(Object^ sender, EventArgs^ e)
	{
		// Open settings page
		settingsForm->ShowDialog();
	}

	Void MainForm::OnLoad(Object^ sender, EventArgs^ e)
	{
		auto hIcon = static_cast<HICON>(LoadImageA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(IDI_ICON1), IMAGE_ICON,
		                                           32, 32, 0));
		this->Icon = Drawing::Icon::FromHandle(static_cast<IntPtr>(hIcon));
		notifyIcon->Icon = this->Icon;
		// DestroyIcon(hIcon);

		// Start setup dialog if game path is invalid in config
		if (String::IsNullOrWhiteSpace(settings->GamePath) || !File::Exists(settings->GamePath)) (gcnew
			SetupForm(settings))->ShowDialog();

		settings->FPSTarget = std::clamp(settings->FPSTarget, tbFPS->Minimum, tbFPS->Maximum); // sanitize

		ckbAutoStart->DataBindings->Add("Checked", settings, "AutoStart");
		tbFPS->DataBindings->Add("Value", settings, "FPSTarget", false, DataSourceUpdateMode::OnPropertyChanged);
		inputFPS->DataBindings->Add("Value", settings, "FPSTarget", false, DataSourceUpdateMode::OnPropertyChanged);

		if (settings->StartMinimized) this->WindowState = FormWindowState::Minimized;
		if (settings->AutoStart) Managed::StartGame(settings);

		this->Focus();

		// Create a thread for applying fps value
		backgroundWorker = gcnew BackgroundWorker();
		backgroundWorker->WorkerReportsProgress = true;
		backgroundWorker->WorkerSupportsCancellation = true;
		backgroundWorker->DoWork += gcnew DoWorkEventHandler(this, &MainForm::OnDoWork);
		backgroundWorker->ProgressChanged += gcnew ProgressChangedEventHandler(this, &MainForm::OnProgressChanged);
		backgroundWorker->RunWorkerAsync();
	}

	Void MainForm::setupMenuItem_Click(Object^ sender, EventArgs^ e)
	{
		auto form = gcnew SetupForm(settings);
		form->ShowDialog();
	}

	Void MainForm::OnDoWork(Object^ sender, DoWorkEventArgs^ e)
	{
		auto worker = safe_cast<BackgroundWorker^>(sender);
		while (!worker->CancellationPending)
		{
			Thread::Sleep(200);
			if (!Unmanaged::IsGameRunning())
				continue;

			// Setup pointer to fps value and vsync value
			if (!Unmanaged::SetupData())
				continue;

			Managed::InjectDLLs(settings->DllList);

			while (!worker->CancellationPending)
			{
				Unmanaged::ApplyFPS(settings->FPSTarget, settings->UsePowerSave);
				Unmanaged::ApplyVSync(settings->AutoDisableVSync);

				if (!Unmanaged::IsGameRunning())
					break;

				Thread::Sleep(1000);
			}

			// ProgressChangedEvent will be dispatched on call to ReportProgress
			// this is needed because any changes to form control need to be done on the thread created it
			// and this method runs on a seperate thread
			if (settings->AutoClose)
				worker->ReportProgress(100);
			else
				worker->ReportProgress(10);
		}
	}

	Void MainForm::OnProgressChanged(Object^ sender, ProgressChangedEventArgs^ e)
	{
		auto progress = e->ProgressPercentage;
		if (progress == 10 || progress == 100) Application::Exit();
	}

	Void MainForm::OnResize(Object^ sender, EventArgs^ e)
	{
		if (this->WindowState == FormWindowState::Minimized)
		{
			// Tray icon visibility and tooltip
			notifyIcon->Visible = true;
			notifyIcon->Text = String::Format("Genshin Impact FPS Unlocker - Limit: {0} FPS", settings->FPSTarget);

			// Only show wintoast notification once
			static bool once = false;
			if (!once)
			{
				notifyIcon->ShowBalloonTip(500);
				once = true;
			}

			// Hide app icon in taskbar
			this->ShowInTaskbar = false;
		}
	}

	Void MainForm::OnDoubleClick(Object^ sender, EventArgs^ e)
	{
		// Restores window and taskbar icon
		this->WindowState = FormWindowState::Normal;
		this->ShowInTaskbar = true;
		this->Activate();
		// notifyIcon->Visible = false;
	}

	Void MainForm::toolStripMenuExit_Click(Object^ sender, EventArgs^ e)
	{
		Application::Exit();
	}

	Void MainForm::OnFormClosing(Object^ sender, FormClosingEventArgs^ e)
	{
		// Save on exit
		settings->Save();
		notifyIcon->Visible = false;
	}

	Void MainForm::menuItemAbout_Click(Object^ sender, EventArgs^ e)
	{
		auto form = gcnew AboutForm();
		form->ShowDialog();
	}

	Void MainForm::mainApp_Click(Object^ sender, EventArgs^ e)
	{
		settings->Save();

		if (!SetCurrentDirectory("C:\\Genshin-Impact-ReShade"))
		{
			MessageBox::Show("Genshin-Impact-ReShade directory was not found in C:\\ disk.",
			                 "Genshin Impact FPS Unlocker", MessageBoxButtons::OK, MessageBoxIcon::Warning);
			return;
		}

		if (File::Exists("C:\\Genshin-Impact-ReShade\\Genshin Stella Mod.exe"))
		{
			Process::Start("C:\\Genshin-Impact-ReShade\\Genshin Stella Mod.exe");
			Application::Exit();
		}
		else
		{
			MessageBox::Show(
				"Cannot find a required file.\n\nPlease reinstall this mod using Genshin Impact Mod Setup.",
				"Genshin Impact FPS Unlocker", MessageBoxButtons::OK, MessageBoxIcon::Warning);
		}
	}

	// System apps
	Void MainForm::si_Click(Object^ sender, EventArgs^ e)
	{
		Process::Start("msinfo32.exe");
	}

	Void MainForm::dxdiag_Click(Object^ sender, EventArgs^ e)
	{
		Process::Start("dxdiag.exe");
	}

	// Links
	Void MainForm::officialWebsite_Click(Object^ sender, EventArgs^ e)
	{
		Process::Start("https://sefinek.net/genshin-impact-reshade");
	}

	Void MainForm::youtube_Click(Object^ sender, EventArgs^ e)
	{
		Process::Start("https://www.youtube.com/@sefinek");
	}

	Void MainForm::githubMainRepo_Click(Object^ sender, EventArgs^ e)
	{
		Process::Start("https://github.com/sefinek24/Genshin-Impact-ReShade");
	}

	Void MainForm::githubFpsUnlock_Click(Object^ sender, EventArgs^ e)
	{
		Process::Start("https://github.com/sefinek24/genshin-fps-unlock");
	}

	Void MainForm::githubMyProfile_Click(Object^ sender, EventArgs^ e)
	{
		Process::Start("https://github.com/sefinek24");
	}

	// Other
	Void MainForm::viewCfg_Click(Object^ sender, EventArgs^ e)
	{
		if (File::Exists("C:\\Genshin-Impact-ReShade\\Data\\Unlocker\\unlocker.config.json"))
		{
			Process::Start("C:\\Genshin-Impact-ReShade\\Data\\Unlocker\\unlocker.config.json");
		}
		else
		{
			MessageBox::Show("I can't find file with name unlocker.config.json.", "Genshin Impact FPS Unlocker",
			                 MessageBoxButtons::OK, MessageBoxIcon::Information);
		}
	}

	Void MainForm::seeCurrentVersion_Click(Object^ sender, EventArgs^ e)
	{
		MessageBox::Show("Config version: v" + settings->ConfigVersion + "\nDate: " + settings->ConfigDate,
		                 "Genshin Impact FPS Unlocker", MessageBoxButtons::OK, MessageBoxIcon::Information);
	}

	// Void MainForm::ToolStipMenu_MouseEnter(Object^ sender, EventArgs^ e)
	// {
	// 	toolStripMenuItem->BackColor = Color::Red;
	// }
}
