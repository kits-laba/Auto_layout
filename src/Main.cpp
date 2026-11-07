#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <random>
#include <cmath>
#include <vector>
#include <algorithm>

using namespace geode::prelude;

// Скорость 1x в GD: ~311.58 юнитов в секунду (1 блок = 30 юнитов).
static constexpr double SPEED = 311.58;
static constexpr int ID_BLOCK = 1;
static constexpr int ID_SPIKE = 8;
static constexpr float GROUND_Y = 15.f;
static constexpr int HOP = 256;   // шаг огибающей, сэмплов
static constexpr double MAX_ANALYZE_SEC = 150.0;

static float snapX(double x) {
	return std::floor(x / 30.0) * 30.f + 15.f;
}

struct SongInfo {
	double bpm = 0;
	double length = 0;      // секунды
	double firstBeat = 0;   // секунды от начала песни до первого удара
};

// Определяет BPM, длину и смещение первого удара по скачанному файлу песни.
static bool analyzeSong(int songId, SongInfo& out, std::string& err) {
	auto mdm = MusicDownloadManager::sharedState();
	if (!mdm->isSongDownloaded(songId)) {
		err = "Песня не скачана. Сначала выбери её в редакторе (Song) и скачай.";
		return false;
	}
	std::string path = mdm->pathForSong(songId);

	auto engine = FMODAudioEngine::sharedEngine();
	if (!engine || !engine->m_system) { err = "FMOD недоступен."; return false; }
	auto sys = engine->m_system;

	FMOD::Sound* snd = nullptr;
	if (sys->createSound(path.c_str(), FMOD_OPENONLY | FMOD_ACCURATETIME, nullptr, &snd) != FMOD_OK || !snd) {
		err = "Не удалось открыть файл песни.";
		return false;
	}

	unsigned int lenMs = 0;
	snd->getLength(&lenMs, FMOD_TIMEUNIT_MS);
	out.length = lenMs / 1000.0;

	FMOD_SOUND_TYPE type;
	FMOD_SOUND_FORMAT fmt;
	int channels = 0, bits = 0;
	float freq = 44100.f;
	snd->getFormat(&type, &fmt, &channels, &bits);
	snd->getDefaults(&freq, nullptr);

	if (fmt != FMOD_SOUND_FORMAT_PCM16 || channels <= 0) {
		snd->release();
		err = "Неподдерживаемый формат песни.";
		return false;
	}

	// Читаем PCM и считаем огибающую громкости.
	std::vector<float> env;
	std::vector<short> buf(16384 * channels);
	double acc = 0;
	int accN = 0;
	size_t framesDone = 0;
	size_t maxFrames = static_cast<size_t>(MAX_ANALYZE_SEC * freq);

	while (framesDone < maxFrames) {
		unsigned int got = 0;
		auto res = snd->readData(buf.data(), static_cast<unsigned int>(buf.size() * sizeof(short)), &got);
		if (got == 0 || (res != FMOD_OK && res != FMOD_ERR_FILE_EOF)) break;
		size_t frames = got / (sizeof(short) * channels);
		for (size_t f = 0; f < frames; f++) {
			double s = 0;
			for (int c = 0; c < channels; c++) s += std::abs(buf[f * channels + c]);
			acc += s / channels;
			if (++accN == HOP) {
				env.push_back(static_cast<float>(acc / HOP));
				acc = 0; accN = 0;
			}
		}
		framesDone += frames;
		if (res == FMOD_ERR_FILE_EOF) break;
	}
	snd->release();

	if (env.size() < 400) { err = "Песня слишком короткая для анализа."; return false; }

	// Онсеты: положительный прирост громкости.
	size_t n = env.size();
	std::vector<double> onset(n, 0.0);
	for (size_t i = 1; i < n; i++) onset[i] = std::max(0.0f, env[i] - env[i - 1]);
	double mean = 0;
	for (double v : onset) mean += v;
	mean /= n;
	for (double& v : onset) v = std::max(0.0, v - mean);

	double fps = freq / HOP;
	int minLag = static_cast<int>(std::floor(fps * 60.0 / 200.0));
	int maxLag = static_cast<int>(std::ceil(fps * 60.0 / 70.0));
	if (maxLag + 2 >= static_cast<int>(n)) { err = "Слишком мало данных."; return false; }

	// Автокорреляция по диапазону 70–200 BPM с мягким предпочтением ~120.
	std::vector<double> ac(maxLag + 2, 0.0);
	int bestLag = minLag;
	double bestScore = -1;
	for (int lag = minLag; lag <= maxLag; lag++) {
		double sum = 0;
		for (size_t i = 0; i + lag < n; i++) sum += onset[i] * onset[i + lag];
		sum /= (n - lag);
		ac[lag] = sum;
		double bpmL = 60.0 * fps / lag;
		double w = std::exp(-0.5 * std::pow(std::log2(bpmL / 120.0) / 0.9, 2));
		if (sum * w > bestScore) { bestScore = sum * w; bestLag = lag; }
	}

	// Параболическая интерполяция для точности.
	double lagF = bestLag;
	if (bestLag > minLag && bestLag < maxLag) {
		double a = ac[bestLag - 1], b = ac[bestLag], c = ac[bestLag + 1];
		double den = a - 2 * b + c;
		if (std::abs(den) > 1e-12) lagF += 0.5 * (a - c) / den;
	}
	out.bpm = std::round(60.0 * fps / lagF * 10.0) / 10.0;

	// Фаза: смещение первого удара.
	double period = lagF;
	double bestPhaseScore = -1, bestPhase = 0;
	for (double p = 0; p < period; p += 1.0) {
		double sum = 0;
		for (double pos = p; pos < n; pos += period) sum += onset[static_cast<size_t>(pos)];
		if (sum > bestPhaseScore) { bestPhaseScore = sum; bestPhase = p; }
	}
	out.firstBeat = bestPhase / fps;
	return true;
}

struct BuildResult {
	int count = 0;
	SongInfo info;
	bool detected = false;
	std::string note;
};

static BuildResult buildLayout(LevelEditorLayer* editor) {
	BuildResult result;
	auto mod = Mod::get();
	int songId = static_cast<int>(mod->getSettingValue<int64_t>("song-id"));
	bool autoDetect = mod->getSettingValue<bool>("auto-detect");
	int density = static_cast<int>(mod->getSettingValue<int64_t>("density"));

	SongInfo info;
	info.bpm = mod->getSettingValue<double>("bpm");
	info.length = mod->getSettingValue<double>("length");
	info.firstBeat = 0.0;

	if (autoDetect && songId > 0) {
		SongInfo detected;
		std::string err;
		if (analyzeSong(songId, detected, err)) {
			info = detected;
			result.detected = true;
		} else {
			result.note = err + " Использованы значения из настроек.";
		}
	}
	result.info = info;

	// Ставим песню в уровень (применится после перезахода в редактор).
	if (songId > 0) {
		editor->m_level->m_songID = songId;
		editor->m_level->m_audioTrack = 0;
	}

	double beatSec = 60.0 / info.bpm;
	double beat = beatSec * SPEED;
	double minGap = density == 1 ? 240.0 : (density == 2 ? 180.0 : 150.0);
	int mult = std::max(1, static_cast<int>(std::ceil(minGap / beat)));
	double step = mult * beat;
	double endX = info.length * SPEED;

	// Первый удар после небольшого разгона (~600 юнитов), строго по сетке битов.
	double startTime = 600.0 / SPEED;
	long k = static_cast<long>(std::ceil((startTime - info.firstBeat) / beatSec));
	if (k < 0) k = 0;
	auto beatX = [&](long idx) { return (info.firstBeat + idx * beatSec) * SPEED; };

	std::mt19937 rng(static_cast<unsigned>(songId) * 2654435761u + 12345u);
	std::uniform_int_distribution<int> roll(0, 99);

	double x = beatX(k);
	while (x < endX) {
		int r = roll(rng);
		double width = 30.0;

		if (r < 55) {
			editor->createObject(ID_SPIKE, ccp(snapX(x), GROUND_Y), false);
			result.count++;
		} else if (r < 80) {
			editor->createObject(ID_SPIKE, ccp(snapX(x), GROUND_Y), false);
			editor->createObject(ID_SPIKE, ccp(snapX(x) + 30.f, GROUND_Y), false);
			width = 60.0;
			result.count += 2;
		} else if (r < 92) {
			for (int i = 0; i < 3; i++)
				editor->createObject(ID_BLOCK, ccp(snapX(x) + 30.f * i, GROUND_Y), false);
			width = 90.0;
			result.count += 3;
		} else if (step >= 210.0) {
			for (int i = 0; i < 3; i++)
				editor->createObject(ID_SPIKE, ccp(snapX(x) + 30.f * i, GROUND_Y), false);
			width = 90.0;
			result.count += 3;
		}

		double prevEnd = x + width;
		k += mult;
		x = beatX(k);
		while (x < prevEnd + 150.0) { k++; x = beatX(k); } // место для прыжка
	}
	return result;
}

class $modify(AutoLayoutPause, EditorPauseLayer) {
	bool init(LevelEditorLayer* editor) {
		if (!EditorPauseLayer::init(editor)) return false;

		auto winSize = CCDirector::sharedDirector()->getWinSize();
		auto menu = CCMenu::create();
		auto spr = ButtonSprite::create("Auto Layout");
		auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AutoLayoutPause::onAutoLayout));
		menu->addChild(btn);
		menu->setPosition(winSize.width - 80.f, 30.f);
		this->addChild(menu);
		return true;
	}

	void onAutoLayout(CCObject*) {
		auto editor = m_editorLayer;
		createQuickPopup(
			"Auto Layout",
			"Построить лайаут по настройкам мода?\n"
			"<cy>ID песни, BPM и плотность: Geode → Auto Layout.</c>",
			"Отмена", "Построить",
			[editor](auto, bool confirmed) {
				if (!confirmed) return;
				auto r = buildLayout(editor);
				std::string text = fmt::format(
					"Объектов: {}\nBPM: {:.1f} ({})\nДлина: {:.1f} c\nПервый удар: {:.2f} c\n",
					r.count, r.info.bpm, r.detected ? "определён" : "из настроек",
					r.info.length, r.info.firstBeat
				);
				if (!r.note.empty()) text += "\n" + r.note;
				text += "\nПерезайди в редактор, чтобы подхватилась песня.";
				FLAlertLayer::create("Готово", text, "OK")->show();
			}
		);
	}
};
