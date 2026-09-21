// Isolated validation harness: compile the real macro.cpp against fake Win32 I/O.
#ifdef NDEBUG
#error "Macro regression tests require assertions enabled"
#endif

#include <cassert>
#include <filesystem>
#include <iostream>
#include "../macro.cpp"

namespace {
int keyDowns(int key) {
	return static_cast<int>(std::count_if(fake_win32::inputs.begin(), fake_win32::inputs.end(),
		[key](const INPUT& input) {
			return input.type == INPUT_KEYBOARD && input.ki.wVk == key &&
				!(input.ki.dwFlags & KEYEVENTF_KEYUP);
		}));
}
void resetInput() {
	fake_win32::tick = 0;
	fake_win32::maxTicks = 1;
	fake_win32::gameActive = true;
	fake_win32::healthy = false;
	std::fill(std::begin(fake_win32::keys), std::end(fake_win32::keys), false);
	fake_win32::inputs.clear();
	fake_win32::beforeTick = {};
	fastLootHoldVKey = 'F';
	fastLootClickVKey = 'L';
	lastFastLootPressed = {};
}
void runLoop() {
	try {
		CoreMacroLoop();
		assert(false);
	} catch (const fake_win32::LoopComplete&) {
	}
}
void writeConfig(const std::string& text) {
	std::ofstream out(GetConfigPath(), std::ios::trunc);
	assert(out.is_open());
	out << text;
}
std::string readFile(const std::string& path) {
	std::ifstream in(path);
	assert(in.is_open());
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void testActivationMatrix() {
	int cases = 0;
	for (bool activeGame : {false, true})
	for (bool activeScript : {false, true})
	for (bool healEnabled : {false, true})
	for (bool independent : {false, true})
	for (bool healthy : {false, true})
	for (int trigger : {-1, 0, 1})
	for (int mouseMask = 0; mouseMask < 4; ++mouseMask) {
		ResetToDefaultConfig();
		resetInput();
		isScriptActive = activeScript;
		globalHealthCheckEnable = healEnabled;
		globalHealthIndependent = independent;
		combatMouseTrigger = trigger;
		spamKeys = {{'1', "1", 50, false, false, false}};
		fake_win32::gameActive = activeGame;
		fake_win32::healthy = healthy;
		fake_win32::keys[VK_LBUTTON] = (mouseMask & 1) != 0;
		fake_win32::keys[VK_RBUTTON] = (mouseMask & 2) != 0;
		runLoop();

		bool combat = trigger == -1 ||
			(trigger == 0 && (mouseMask & 1)) || (trigger == 1 && (mouseMask & 2));
		bool expectHeal = activeGame && activeScript && healEnabled && !healthy &&
			(independent || combat);
		bool expectSpam = activeGame && activeScript && combat;
		assert(keyDowns('Q') == static_cast<int>(expectHeal));
		assert(keyDowns('1') == static_cast<int>(expectSpam));
		++cases;
	}
	assert(cases == 384);
	std::cout << "PASS: 384 activation combinations, healing and combat spam checked\n";
}

void testCooldownAndLiveChanges() {
	ResetToDefaultConfig();
	resetInput();
	globalHealthIndependent = true;
	isScriptActive = true;
	healthDelayMs = 1;
	fake_win32::maxTicks = 3;
	fake_win32::beforeTick = [](int tick) {
		if (tick == 1) healthDelayMs = 60000;
	};
	runLoop();
	assert(keyDowns('Q') == 1);
	assert(keyDowns('1') == 0);

	resetInput();
	healthDelayMs = 1;
	fake_win32::maxTicks = 3;
	runLoop();
	assert(keyDowns('Q') == 3);

	resetInput();
	globalHealthIndependent = false;
	fake_win32::maxTicks = 3;
	fake_win32::beforeTick = [](int tick) {
		if (tick == 1) globalHealthIndependent = true;
		if (tick == 2) isScriptActive = false;
	};
	runLoop();
	assert(keyDowns('Q') == 1);
	std::cout << "PASS: heal cooldown and live independent-mode/script toggles\n";
}

void testFastLootUnchanged() {
	for (bool independent : {false, true}) {
		ResetToDefaultConfig();
		resetInput();
		globalHealthIndependent = independent;
		isScriptActive = true;
		fake_win32::healthy = true;
		fake_win32::keys['F'] = true;
		runLoop();
		assert(keyDowns('Q') == 0);
		assert(keyDowns('1') == 0);
		assert(keyDowns('L') == 1);
	}
	std::cout << "PASS: independent auto-heal does not change fast loot\n";
}

void testProfilesAndPersistence() {
	ResetToDefaultConfig();
	assert(!ProfileConfig{}.globalHealthIndependent);
	assert(!MacroSettingsSnapshot{}.globalHealthIndependent);
	assert(!globalHealthIndependent);
	assert(!GetMacroSettingsSnapshot().globalHealthIndependent);
	globalHealthIndependent = true;
	assert(GetMacroSettingsSnapshot().globalHealthIndependent);
	auto profile = MakeProfileFromGlobals("Independent");
	assert(profile.globalHealthIndependent);
	globalHealthIndependent = false;
	ApplyProfileToGlobals(profile);
	assert(globalHealthIndependent);

	SaveConfig();
	AddProfile();
	assert(activeProfileIndex == 1 && globalHealthIndependent);
	globalHealthIndependent = false;
	SaveConfig();
	SelectProfile(0);
	assert(globalHealthIndependent);
	SelectProfile(1);
	assert(!globalHealthIndependent);
	auto text = readFile(GetConfigPath());
	assert(text.find("profile.0.globalHealthIndependent=1\n") != std::string::npos);
	assert(text.find("profile.1.globalHealthIndependent=0\n") != std::string::npos);
	ResetToDefaultConfig();
	LoadConfig();
	assert(profiles.size() == 2 && activeProfileIndex == 1);
	assert(profiles[0].globalHealthIndependent);
	assert(!profiles[1].globalHealthIndependent);
	assert(!globalHealthIndependent);
	SelectProfile(0);
	assert(globalHealthIndependent && GetMacroSettingsSnapshot().globalHealthIndependent);
	DeleteActiveProfile();
	assert(profiles.size() == 1 && !globalHealthIndependent);
	globalHealthIndependent = true;
	ResetToDefaultConfig();
	assert(!globalHealthIndependent && !profiles[0].globalHealthIndependent);

	// The master switch remains distinct; disabled healing can retain its preference.
	globalHealthIndependent = true;
	globalHealthCheckEnable = false;
	SaveConfig();
	ResetToDefaultConfig();
	LoadConfig();
	assert(globalHealthIndependent && !globalHealthCheckEnable);
	std::cout << "PASS: defaults, snapshots, profile clone/switch/delete, config round-trip\n";
}

void testConfigCompatibility() {
	for (int version : {2, 3}) {
		writeConfig("version=" + std::to_string(version) +
			"\nprofileCount=2\nactiveProfile=0\nprofile.0.combatMouseTrigger=0\n"
			"profile.1.combatMouseTrigger=1\n");
		globalHealthIndependent = true;
		LoadConfig();
		assert(!globalHealthIndependent);
		assert(!profiles[0].globalHealthIndependent && !profiles[1].globalHealthIndependent);
		assert(combatMouseTrigger == 0);
		SelectProfile(1);
		assert(!globalHealthIndependent && combatMouseTrigger == 1);
	}
	for (const auto& [value, expected] : std::vector<std::pair<std::string, bool>>{
		{"1", true}, {"true", true}, {" ON ", true}, {"0", false},
		{"false", false}, {"off", false}, {"invalid", false}, {"", false}}) {
		writeConfig("version=3\nprofileCount=1\nprofile.0.globalHealthIndependent=" + value + "\n");
		globalHealthIndependent = !expected;
		LoadConfig();
		assert(globalHealthIndependent == expected);
	}
	writeConfig("6 Mouse5 116 F5 1\n1 81 Q 50 960 1010\n1\n49 1 50 0 0 0\n");
	globalHealthIndependent = true;
	LoadConfig();
	assert(!globalHealthIndependent && !profiles[0].globalHealthIndependent);
	assert(combatMouseTrigger == 1 && healthVKey == 'Q');
	assert(spamKeys.size() == 1 && spamKeys[0].vKey == '1');
	assert(readFile(GetConfigPath()).find("profile.0.globalHealthIndependent=0\n") != std::string::npos);
	std::cout << "PASS: missing/invalid option defaults off; v2/v3 and legacy migration\n";
}

void testLanguages() {
	fake_win32::resources[IDR_LANG_EN] = readFile("lang_en.txt");
	fake_win32::resources[IDR_LANG_RU] = readFile("lang_ru.txt");
	lang = LocStrings{};
	assert(lang.chkGlobalHealthIndependent == "Independent operation");
	fake_win32::language = LANG_RUSSIAN;
	LoadLanguage();
	assert(lang.chkGlobalHealthIndependent == "Независимая работа");
	fake_win32::language = 9;
	LoadLanguage();
	assert(lang.chkGlobalHealthIndependent == "Independent operation");
	fake_win32::resources.erase(IDR_LANG_RU);
	fake_win32::language = LANG_RUSSIAN;
	lang.chkGlobalHealthIndependent = "";
	LoadLanguage();
	assert(lang.chkGlobalHealthIndependent == "Independent operation");
	std::cout << "PASS: actual language loader, Russian/English label and fallback\n";
}
}

int main() {
	auto temp = std::filesystem::temp_directory_path() /
		("d4rt-regression-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(temp);
	fake_win32::appData = temp.string() + "/";
	testActivationMatrix();
	testCooldownAndLiveChanges();
	testFastLootUnchanged();
	testProfilesAndPersistence();
	testConfigCompatibility();
	testLanguages();
	std::filesystem::remove_all(temp);
	std::cout << "All isolated macro regression tests passed.\n";
}
