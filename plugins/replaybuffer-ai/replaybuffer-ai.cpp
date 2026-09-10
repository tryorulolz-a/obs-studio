#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QWidget>

#include "../../frontend/replaybuffer/ReplayBufferAIEditor.hpp"

OBS_DECLARE_MODULE()

static ReplayBufferAIEditor *g_editor = nullptr;

MODULE_EXPORT const char *obs_module_description(void)
{
	return "ReplayBuffer local AI editing dock for OBS Studio";
}

bool obs_module_load(void)
{
	QWidget *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	g_editor = new ReplayBufferAIEditor(mainWindow);
	if (!obs_frontend_add_dock_by_id("replaybuffer_ai_editor", "AI Editor", g_editor)) {
		delete g_editor;
		g_editor = nullptr;
		blog(LOG_ERROR, "ReplayBuffer AI: failed to register AI Editor dock");
		return false;
	}

	blog(LOG_INFO, "ReplayBuffer AI: AI Editor dock registered");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_dock("replaybuffer_ai_editor");
	g_editor = nullptr;
}
