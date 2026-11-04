// roundtrip.cpp - find NES state that save states do not capture.
//
// For each ROM:
//   1. console A: load the ROM, run W frames, take a save state S (the same
//      Serializer stream Emulator::Serialize uses).
//   2. A runs K more frames; its full state (Text serializer format) is recorded
//      after every frame.
//   3. console B: a *fresh* NesConsole loads the same ROM, loads S, and runs the
//      same K frames.  Its state is compared with A's after every frame.
// Anything that affects emulation but is not in the save state makes B diverge
// from A.  Such a bug also breaks rewind, netplay, movies and the history viewer.
// The first divergent frame and the first differing state fields are reported.
//
// No input is applied, so the ROMs must run on their own (test ROMs do).
//
// Usage: roundtrip [--warmup W] [--frames K] [--lines N] <rom.nes | dir> ...

#include "pch.h"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <fstream>
#include <map>
// Test-tool-only access to Emulator::_isRunAheadFrame (see main). Standard headers
// are already included via pch.h, so only MesenCE headers see this.
#define private public
#include "Shared/Emulator.h"
#undef private
#include "Shared/SaveStateManager.h"
#include "NES/NesConsole.h"
#include "Utilities/Serializer.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/FolderUtilities.h"

namespace fs = std::filesystem;

static string SaveState(NesConsole& c, SerializeFormat format)
{
	Serializer s(SaveStateManager::FileFormatVersion, true, format);
	s.Stream(c, "", -1);
	std::stringstream out;
	s.SaveTo(out, 0);
	return out.str();
}

static bool LoadState(NesConsole& c, const string& data)
{
	Serializer s(SaveStateManager::FileFormatVersion, false);
	std::stringstream in(data);
	if(!s.LoadFrom(in)) {
		return false;
	}
	s.Stream(c, "", -1);
	return true;
}

static vector<uint8_t> ReadFile(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// Parse the uncompressed binary save-state stream (a 1-byte compression flag, then
// repeated: key '\0', uint32 size, size bytes) into key -> value bytes, in order.
static vector<std::pair<string, string>> ParseState(const string& data)
{
	vector<std::pair<string, string>> fields;
	size_t pos = 1;
	while(pos < data.size()) {
		size_t end = data.find('\0', pos);
		if(end == string::npos || end + 5 > data.size()) {
			break;
		}
		string key = data.substr(pos, end - pos);
		uint32_t size;
		memcpy(&size, data.data() + end + 1, 4);
		pos = end + 5;
		fields.push_back({ key, data.substr(pos, size) });
		pos += size;
	}
	return fields;
}

// BaseMapper stores an inaccessible page either as {type, offset, NoAccess}
// (SetCpuMemoryMapping with NoAccess) or as the "unmapped" sentinel
// (RemoveCpuMemoryMapping). RestorePrgChrState turns the former into the latter on
// load, so the bytes differ although behaviour is identical (both read open bus).
// Blank out type/offset for NoAccess pages so only behavioural differences remain.
static void Canonicalize(vector<std::pair<string, string>>& fields)
{
	std::map<string, string*> byKey;
	for(auto& f : fields) {
		byKey[f.first] = &f.second;
	}
	for(const char* bus : { "prg", "chr" }) {
		string* access = byKey[string("mapper.") + bus + "MemoryAccess"];
		string* type = byKey[string("mapper.") + bus + "MemoryType"];
		string* offset = byKey[string("mapper.") + bus + "MemoryOffset"];
		if(!access || !type || !offset) {
			continue;
		}
		size_t pages = bus[0] == 'p' ? 0x100 : 0x40;
		if(access->size() % pages || type->size() % pages || offset->size() % pages) {
			continue;
		}
		size_t aw = access->size() / pages, tw = type->size() / pages, ow = offset->size() / pages;
		for(size_t i = 0; i < pages; i++) {
			bool noAccess = true;
			for(size_t b = 0; b < aw; b++) {
				noAccess &= (*access)[i * aw + b] == 0;
			}
			if(noAccess) {
				std::fill(type->begin() + i * tw, type->begin() + (i + 1) * tw, 0);
				std::fill(offset->begin() + i * ow, offset->begin() + (i + 1) * ow, 0);
			}
		}
	}
}

static bool SameBehaviour(const string& a, const string& b)
{
	if(a == b) {
		return true;
	}
	auto fa = ParseState(a), fb = ParseState(b);
	Canonicalize(fa);
	Canonicalize(fb);
	return fa == fb;
}

static string Describe(const string& expected, const string& actual)
{
	std::ostringstream ss;
	if(expected.size() != actual.size()) {
		ss << "size " << expected.size() << " vs " << actual.size();
		return ss.str();
	}
	if(expected.size() <= 8) {
		uint64_t e = 0, a = 0;
		memcpy(&e, expected.data(), expected.size());
		memcpy(&a, actual.data(), actual.size());
		ss << "expected " << e << ", actual " << a;
		return ss.str();
	}
	size_t first = 0, count = 0;
	for(size_t i = 0; i < expected.size(); i++) {
		if(expected[i] != actual[i]) {
			if(!count) {
				first = i;
			}
			count++;
		}
	}
	ss << count << " of " << expected.size() << " bytes differ, first at +" << first << " (expected "
		<< (int)(uint8_t)expected[first] << ", actual " << (int)(uint8_t)actual[first] << ")";
	return ss.str();
}

int main(int argc, char* argv[])
{
	int warmup = 60, frames = 120, maxLines = 8;
	vector<fs::path> roms;
	for(int i = 1; i < argc; i++) {
		string a = argv[i];
		if(a == "--warmup" && i + 1 < argc) {
			warmup = std::stoi(argv[++i]);
		} else if(a == "--frames" && i + 1 < argc) {
			frames = std::stoi(argv[++i]);
		} else if(a == "--lines" && i + 1 < argc) {
			maxLines = std::stoi(argv[++i]);
		} else if(fs::is_directory(a)) {
			for(const auto& e : fs::directory_iterator(a)) {
				if(e.path().extension() == ".nes") {
					roms.push_back(e.path());
				}
			}
		} else {
			roms.push_back(a);
		}
	}
	std::sort(roms.begin(), roms.end());
	if(roms.empty()) {
		std::cerr << "usage: roundtrip [--warmup W] [--frames K] [--lines N] <rom.nes | dir> ...\n";
		return 2;
	}

	fs::path home = fs::temp_directory_path() / "mesen-roundtrip-home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	Emulator emu;
	emu.Initialize(false);
	// Consoles here are driven directly (no emulation thread, no video decoder thread).
	// Run-ahead mode emulates frames fully but skips frame/audio output, which is what
	// a headless frame loop needs: otherwise VideoDecoder::UpdateFrame waits forever
	// for a decode thread that is not running.
	emu._isRunAheadFrame = true;

	int ok = 0, diverged = 0, skipped = 0;
	for(const fs::path& rom : roms) {
		string name = rom.filename().string();
		vector<uint8_t> romData = ReadFile(rom);
		VirtualFile fileA(romData.data(), romData.size(), name);
		NesConsole a(&emu);
		if(a.LoadRom(fileA) != LoadRomResult::Success) {
			std::cout << "SKIP  " << name << " (load failed)\n";
			skipped++;
			continue;
		}
		for(int i = 0; i < warmup; i++) {
			a.RunFrame();
		}
		string snapshot = SaveState(a, SerializeFormat::Binary);
		vector<string> expected;
		expected.reserve(frames);
		for(int i = 0; i < frames; i++) {
			a.RunFrame();
			expected.push_back(SaveState(a, SerializeFormat::Binary));
		}

		VirtualFile fileB(romData.data(), romData.size(), name);
		NesConsole b(&emu);
		if(b.LoadRom(fileB) != LoadRomResult::Success || !LoadState(b, snapshot)) {
			std::cout << "SKIP  " << name << " (state load failed)\n";
			skipped++;
			continue;
		}
		int badFrame = -1;
		string actual;
		for(int i = 0; i < frames; i++) {
			b.RunFrame();
			actual = SaveState(b, SerializeFormat::Binary);
			if(!SameBehaviour(expected[i], actual)) {
				badFrame = i;
				break;
			}
		}

		if(badFrame < 0) {
			ok++;
			continue;
		}
		diverged++;
		std::cout << "DIVERGE " << name << " at frame +" << (badFrame + 1) << " after load\n";
		auto fa = ParseState(expected[badFrame]);
		auto fb = ParseState(actual);
		Canonicalize(fa);
		Canonicalize(fb);
		std::map<string, string> mb(fb.begin(), fb.end());
		int shown = 0;
		for(auto& [key, value] : fa) {
			auto it = mb.find(key);
			if(it == mb.end()) {
				std::cout << "    " << key << ": missing after load\n";
			} else if(it->second != value) {
				std::cout << "    " << key << ": " << Describe(value, it->second) << "\n";
			} else {
				continue;
			}
			if(++shown >= maxLines) {
				break;
			}
		}
	}
	std::cout << "TOTAL: " << ok << " consistent, " << diverged << " diverged, " << skipped << " skipped (of " << roms.size()
				 << ")\n";
	return diverged ? 1 : 0;
}
