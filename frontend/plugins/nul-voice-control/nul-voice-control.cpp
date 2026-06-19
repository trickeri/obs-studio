/*
    Nuldrums / NulStream: voice control plugin.
    SPDX-License-Identifier: GPL-2.0-or-later

    Self-contained frontend plugin that listens to the universal voicechat
    dictation daemon and maps spoken commands to OBS frontend actions. Acts only
    on transcripts voicechat marked mode=="emit" (i.e. NulStream was focused and
    voicechat deliberately withheld the paste). Every transcript is logged.

    Implemented commands (v1):
      - Scene switching:   "switch to <scene>" / "go to <scene>" / "scene <scene>"
      - Recording control: "start/stop/pause/resume recording"
      - Source mute:       "mute <source>" / "unmute <source>"
      - Source visibility: "show <source>" / "hide <source>"  (current scene)

    See ~/programming/OBS/AIPlans/voicechat-integration.md for the socket contract.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QObject>
#include <QString>
#include <QStringList>

#include "nul-voicechat-listener.hpp"

OBS_DECLARE_MODULE()

namespace {

#define VLOG(level, fmt, ...) blog(level, "[nul-voice] " fmt, ##__VA_ARGS__)

/* Case-insensitive match score between a spoken query and a real OBS name:
 * 2 = equal, 1 = one contains the other, 0 = no match. */
int matchScore(const QString &query, const QString &name)
{
	const QString q = query.trimmed().toLower();
	const QString n = name.trimmed().toLower();
	if (q.isEmpty() || n.isEmpty())
		return 0;
	if (q == n)
		return 2;
	if (n.contains(q) || q.contains(n))
		return 1;
	return 0;
}

/* Find the best-matching scene name among the frontend's scenes. */
QString bestSceneName(const QString &query)
{
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	QString best;
	int bestScore = 0;
	for (size_t i = 0; i < scenes.sources.num; i++) {
		const char *cname = obs_source_get_name(scenes.sources.array[i]);
		if (!cname)
			continue;
		const QString name = QString::fromUtf8(cname);
		const int score = matchScore(query, name);
		if (score > bestScore) {
			bestScore = score;
			best = name;
		}
	}
	obs_frontend_source_list_free(&scenes);
	return best;
}

struct SourceMatch {
	QString query;
	QString best;
	int bestScore = 0;
};

/* Find the best-matching source name across all sources (used for mute/unmute). */
QString bestSourceName(const QString &query)
{
	SourceMatch m{query, QString(), 0};
	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			auto *mm = static_cast<SourceMatch *>(param);
			const char *cname = obs_source_get_name(source);
			if (cname) {
				const QString name = QString::fromUtf8(cname);
				const int score = matchScore(mm->query, name);
				if (score > mm->bestScore) {
					mm->bestScore = score;
					mm->best = name;
				}
			}
			return true;
		},
		&m);
	return m.best;
}

struct ItemMatch {
	QString query;
	obs_sceneitem_t *best = nullptr;
	int bestScore = 0;
};

/* Set visibility of the best-matching scene item in the current scene. */
bool setSourceVisibleInCurrentScene(const QString &query, bool visible)
{
	obs_source_t *sceneSource = obs_frontend_get_current_scene();
	if (!sceneSource)
		return false;
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		return false;
	}

	ItemMatch m{query, nullptr, 0};
	/* The scene ref is held for the duration, so item pointers stay valid. */
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *mm = static_cast<ItemMatch *>(param);
			obs_source_t *src = obs_sceneitem_get_source(item);
			const char *cname = src ? obs_source_get_name(src) : nullptr;
			if (cname) {
				const int score = mm->query.isEmpty()
							  ? 0
							  : matchScore(mm->query, QString::fromUtf8(cname));
				if (score > mm->bestScore) {
					mm->bestScore = score;
					mm->best = item;
				}
			}
			return true;
		},
		&m);

	bool ok = false;
	if (m.best) {
		obs_sceneitem_set_visible(m.best, visible);
		ok = true;
	}
	obs_source_release(sceneSource);
	return ok;
}

bool muteSource(const QString &query, bool muted)
{
	const QString name = bestSourceName(query);
	if (name.isEmpty())
		return false;
	obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
	if (!src)
		return false;
	obs_source_set_muted(src, muted);
	obs_source_release(src);
	VLOG(LOG_INFO, "%s source: %s", muted ? "muted" : "unmuted", name.toUtf8().constData());
	return true;
}

bool switchScene(const QString &query)
{
	const QString name = bestSceneName(query);
	if (name.isEmpty()) {
		VLOG(LOG_WARNING, "no scene matching \"%s\"", query.toUtf8().constData());
		return false;
	}
	obs_source_t *scene = obs_get_source_by_name(name.toUtf8().constData());
	if (!scene)
		return false;
	obs_frontend_set_current_scene(scene);
	obs_source_release(scene);
	VLOG(LOG_INFO, "switched to scene: %s", name.toUtf8().constData());
	return true;
}

/* If cmd starts with any of the given prefixes (+space), return the remainder. */
QString afterPrefix(const QString &cmd, const QStringList &prefixes)
{
	for (const QString &p : prefixes) {
		const QString pre = p + QStringLiteral(" ");
		if (cmd.startsWith(pre))
			return cmd.mid(pre.size()).trimmed();
	}
	return QString();
}

class VoiceController : public QObject {
public:
	explicit VoiceController(QObject *parent = nullptr) : QObject(parent)
	{
		m_listener = new NulVoiceChatListener(this);
		connect(m_listener, &NulVoiceChatListener::transcriptReceived, this,
			&VoiceController::onTranscript);
		VLOG(LOG_INFO, "listening on %s",
		     NulVoiceChatListener::socketPath().toUtf8().constData());
	}

private:
	void onTranscript(const QString &text, const QString &app, const QString &mode)
	{
		VLOG(LOG_INFO, "heard \"%s\" (app=%s mode=%s)", text.toUtf8().constData(),
		     app.toUtf8().constData(), mode.toUtf8().constData());
		if (mode != QStringLiteral("emit"))
			return;

		QString cmd = text.trimmed().toLower();
		while (!cmd.isEmpty() && QStringLiteral(".!?,").contains(cmd.back()))
			cmd.chop(1);
		if (cmd.isEmpty())
			return;

		if (dispatch(cmd))
			return;
		VLOG(LOG_INFO, "no command matched: \"%s\"", cmd.toUtf8().constData());
	}

	bool dispatch(const QString &cmd)
	{
		/* Recording first, so "stop recording" isn't read as a scene name. */
		if (cmd == QStringLiteral("start recording") || cmd == QStringLiteral("begin recording")) {
			obs_frontend_recording_start();
			VLOG(LOG_INFO, "recording started");
			return true;
		}
		if (cmd == QStringLiteral("stop recording") || cmd == QStringLiteral("end recording")) {
			obs_frontend_recording_stop();
			VLOG(LOG_INFO, "recording stopped");
			return true;
		}
		if (cmd == QStringLiteral("pause recording")) {
			obs_frontend_recording_pause(true);
			VLOG(LOG_INFO, "recording paused");
			return true;
		}
		if (cmd == QStringLiteral("resume recording") || cmd == QStringLiteral("unpause recording")) {
			obs_frontend_recording_pause(false);
			VLOG(LOG_INFO, "recording resumed");
			return true;
		}

		/* Scene switching (checked before "show <source>" so "show scene X" wins). */
		const QString scene = afterPrefix(
			cmd, {QStringLiteral("switch to scene"), QStringLiteral("switch to"),
			      QStringLiteral("switch scene to"), QStringLiteral("go to scene"),
			      QStringLiteral("go to"), QStringLiteral("change scene to"),
			      QStringLiteral("change to scene"), QStringLiteral("show scene"),
			      QStringLiteral("scene")});
		if (!scene.isEmpty())
			return switchScene(scene);

		/* Mute / unmute. */
		const QString unmuteName = afterPrefix(cmd, {QStringLiteral("unmute")});
		if (!unmuteName.isEmpty())
			return muteSource(unmuteName, false);
		const QString muteName = afterPrefix(cmd, {QStringLiteral("mute")});
		if (!muteName.isEmpty())
			return muteSource(muteName, true);

		/* Show / hide a source in the current scene. */
		const QString showName = afterPrefix(cmd, {QStringLiteral("show source"), QStringLiteral("show")});
		if (!showName.isEmpty()) {
			if (setSourceVisibleInCurrentScene(showName, true)) {
				VLOG(LOG_INFO, "showed source: %s", showName.toUtf8().constData());
				return true;
			}
			VLOG(LOG_WARNING, "no source matching \"%s\" in current scene",
			     showName.toUtf8().constData());
			return false;
		}
		const QString hideName = afterPrefix(cmd, {QStringLiteral("hide source"), QStringLiteral("hide")});
		if (!hideName.isEmpty()) {
			if (setSourceVisibleInCurrentScene(hideName, false)) {
				VLOG(LOG_INFO, "hid source: %s", hideName.toUtf8().constData());
				return true;
			}
			VLOG(LOG_WARNING, "no source matching \"%s\" in current scene",
			     hideName.toUtf8().constData());
			return false;
		}

		return false;
	}

	NulVoiceChatListener *m_listener = nullptr;
};

VoiceController *g_controller = nullptr;

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		if (!g_controller)
			g_controller = new VoiceController();
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		delete g_controller;
		g_controller = nullptr;
	}
}

} // namespace

bool obs_module_load(void)
{
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	VLOG(LOG_INFO, "NulStream voice control loaded");
	return true;
}

void obs_module_unload(void)
{
	delete g_controller;
	g_controller = nullptr;
}
