/******************************************************************************
    Copyright (C) 2013-2015 by Hugh Bailey <obs.jim@gmail.com>
                               Zachary Lund <admin@computerquip.com>
                               Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <time.h>
#include <obs.hpp>
#include <QGuiApplication>
#include <QMessageBox>
#include <QShowEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QDesktopWidget>
#include <QRect>
#include <QScreen>

#include <util/dstr.h>
#include <util/util.hpp>
#include <util/platform.h>
#include <util/profiler.hpp>
#include <util/dstr.hpp>
#include <graphics/math-defs.h>

#include "obs-app.hpp"
#include "platform.hpp"
#include "visibility-item-widget.hpp"
#include "item-widget-helpers.hpp"
#include "window-basic-settings.hpp"
#include "window-namedialog.hpp"
//#include "window-basic-auto-config.hpp"
#include "window-basic-source-select.hpp"
#include "window-basic-main.hpp"
#include "window-basic-stats.hpp"
#include "window-basic-main-outputs.hpp"
#include "window-basic-properties.hpp"
//#include "window-log-reply.hpp"
#include "window-projector.hpp"
//#include "window-remux.hpp"
#include "qt-wrappers.hpp"
#include "display-helpers.hpp"
#include "volume-control.hpp"

#include "../Student/common/rtp.h"
#include "window-view-ptz.h"

#define VOLUME_METER_DECAY_FAST        23.53
#define VOLUME_METER_DECAY_MEDIUM      11.76
#define VOLUME_METER_DECAY_SLOW        8.57

#if defined(_WIN32) && defined(ENABLE_WIN_UPDATER)
#include "win-update.hpp"
#endif

#include "ui_OBSBasic.h"

#include <fstream>
#include <sstream>

#include <QScreen>
#include <QWindow>

using namespace std;

namespace {

template <typename OBSRef>
struct SignalContainer {
	OBSRef ref;
	vector<shared_ptr<OBSSignal>> handlers;
};

}

Q_DECLARE_METATYPE(OBSScene);
Q_DECLARE_METATYPE(OBSSceneItem);
Q_DECLARE_METATYPE(OBSSource);
Q_DECLARE_METATYPE(obs_order_movement);
Q_DECLARE_METATYPE(SignalContainer<OBSScene>);

template <typename T>
static T GetOBSRef(QListWidgetItem *item)
{
	return item->data(static_cast<int>(QtDataRole::OBSRef)).value<T>();
}

template <typename T>
static void SetOBSRef(QListWidgetItem *item, T &&val)
{
	item->setData(static_cast<int>(QtDataRole::OBSRef),
			QVariant::fromValue(val));
}

static void AddExtraModulePaths()
{
	char base_module_dir[512];
#if defined(_WIN32) || defined(__APPLE__)
	int ret = GetProgramDataPath(base_module_dir, sizeof(base_module_dir), "obs-studio/plugins/%module%");
#else
	int ret = GetConfigPath(base_module_dir, sizeof(base_module_dir), "obs-studio/plugins/%module%");
#endif

	if (ret <= 0)
		return;

	string path = (char*)base_module_dir;
#if defined(__APPLE__)
	obs_add_module_path((path + "/bin").c_str(), (path + "/data").c_str());

	BPtr<char> config_bin = os_get_config_path_ptr("obs-studio/plugins/%module%/bin");
	BPtr<char> config_data = os_get_config_path_ptr("obs-studio/plugins/%module%/data");
	obs_add_module_path(config_bin, config_data);

#elif ARCH_BITS == 64
	obs_add_module_path((path + "/bin/64bit").c_str(),
			(path + "/data").c_str());
#else
	obs_add_module_path((path + "/bin/32bit").c_str(),
			(path + "/data").c_str());
#endif
}

OBSBasic::OBSBasic(QWidget *parent)
	: OBSMainWindow  (parent),
	  ui             (new Ui::OBSBasic)
{
	setAttribute(Qt::WA_NativeWindow);
	setAcceptDrops(true);
	ui->setupUi(this);

	// 设置窗口图标 => 必须用png图片 => 解决有些机器不认ico，造成左上角图标无法显示...
	this->setWindowIcon(QIcon(":/res/images/obs.png"));

	// 强制隐藏菜单栏和预览禁止...
	ui->menubar->setVisible(false);
	ui->previewDisabledLabel->setVisible(false);

	startingDockLayout = saveState();

	copyActionsDynamicProperties();

	ui->sources->setItemDelegate(new VisibilityItemDelegate(ui->sources));

	char styleSheetPath[512];
	int ret = GetProfilePath(styleSheetPath, sizeof(styleSheetPath),
			"stylesheet.qss");
	if (ret > 0) {
		if (QFile::exists(styleSheetPath)) {
			QString path = QString("file:///") +
				QT_UTF8(styleSheetPath);
			App()->setStyleSheet(path);
		}
	}

	qRegisterMetaType<OBSScene>    ("OBSScene");
	qRegisterMetaType<OBSSceneItem>("OBSSceneItem");
	qRegisterMetaType<OBSSource>   ("OBSSource");
	qRegisterMetaType<obs_hotkey_id>("obs_hotkey_id");

	qRegisterMetaTypeStreamOperators<
		std::vector<std::shared_ptr<OBSSignal>>>(
				"std::vector<std::shared_ptr<OBSSignal>>");
	qRegisterMetaTypeStreamOperators<OBSScene>("OBSScene");
	qRegisterMetaTypeStreamOperators<OBSSceneItem>("OBSSceneItem");

	ui->scenes->setAttribute(Qt::WA_MacShowFocusRect, false);
	ui->sources->setAttribute(Qt::WA_MacShowFocusRect, false);

	auto displayResize = [this]() {
		struct obs_video_info ovi;

		if (obs_get_video_info(&ovi))
			ResizePreview(ovi.base_width, ovi.base_height);
	};

	connect(windowHandle(), &QWindow::screenChanged, displayResize);
	connect(ui->preview, &OBSQTDisplay::DisplayResized, displayResize);

	installEventFilter(CreateShortcutFilter());

	stringstream name;
	name << "OBS " << App()->GetVersionString();
	blog(LOG_INFO, "%s", name.str().c_str());
	blog(LOG_INFO, "---------------------------------");

	UpdateTitleBar();

	connect(ui->scenes->itemDelegate(),
			SIGNAL(closeEditor(QWidget*,
					QAbstractItemDelegate::EndEditHint)),
			this,
			SLOT(SceneNameEdited(QWidget*,
					QAbstractItemDelegate::EndEditHint)));

	connect(ui->sources->itemDelegate(),
			SIGNAL(closeEditor(QWidget*,
					QAbstractItemDelegate::EndEditHint)),
			this,
			SLOT(SceneItemNameEdited(QWidget*,
					QAbstractItemDelegate::EndEditHint)));

	cpuUsageInfo = os_cpu_usage_info_start();
	cpuUsageTimer = new QTimer(this);
	connect(cpuUsageTimer, SIGNAL(timeout()),
			ui->statusbar, SLOT(UpdateCPUUsage()));
	cpuUsageTimer->start(3000);

#ifdef __APPLE__
	ui->actionRemoveSource->setShortcuts({Qt::Key_Backspace});
	ui->actionRemoveScene->setShortcuts({Qt::Key_Backspace});

	ui->action_Settings->setMenuRole(QAction::PreferencesRole);
	ui->actionE_xit->setMenuRole(QAction::QuitRole);
#endif

	auto addNudge = [this](const QKeySequence &seq, const char *s)
	{
		QAction *nudge = new QAction(ui->preview);
		nudge->setShortcut(seq);
		nudge->setShortcutContext(Qt::WidgetShortcut);
		ui->preview->addAction(nudge);
		connect(nudge, SIGNAL(triggered()), this, s);
	};

	addNudge(Qt::Key_Up, SLOT(NudgeUp()));
	addNudge(Qt::Key_Down, SLOT(NudgeDown()));
	addNudge(Qt::Key_Left, SLOT(NudgeLeft()));
	addNudge(Qt::Key_Right, SLOT(NudgeRight()));

	auto assignDockToggle = [this](QDockWidget *dock, QAction *action)
	{
		auto handleWindowToggle = [action] (bool vis)
		{
			action->blockSignals(true);
			action->setChecked(vis);
			action->blockSignals(false);
		};
		auto handleMenuToggle = [dock] (bool check)
		{
			dock->blockSignals(true);
			dock->setVisible(check);
			dock->blockSignals(false);
		};

		dock->connect(dock->toggleViewAction(), &QAction::toggled,
				handleWindowToggle);
		dock->connect(action, &QAction::toggled,
				handleMenuToggle);
	};

	assignDockToggle(ui->scenesDock, ui->toggleScenes);
	assignDockToggle(ui->sourcesDock, ui->toggleSources);
	assignDockToggle(ui->mixerDock, ui->toggleMixer);
	assignDockToggle(ui->controlsDock, ui->toggleControls);
	assignDockToggle(ui->transitionsDock, ui->toggleTransitions);

	//hide all docking panes
	ui->toggleScenes->setChecked(false);
	ui->toggleSources->setChecked(false);
	ui->toggleMixer->setChecked(false);
	ui->toggleTransitions->setChecked(false);
	ui->toggleControls->setChecked(false);

	//restore parent window geometry
	const char *geometry = config_get_string(App()->GlobalConfig(), "BasicWindow", "geometry");
	if (geometry != NULL) {
		QByteArray byteArray = QByteArray::fromBase64(QByteArray(geometry));
		restoreGeometry(byteArray);

		QRect windowGeometry = normalGeometry();
		if (!WindowPositionValid(windowGeometry)) {
			QRect rect = App()->desktop()->geometry();
			setGeometry(QStyle::alignedRect(
						Qt::LeftToRight,
						Qt::AlignCenter,
						size(), rect));
		}
	}
}

static void SaveAudioDevice(const char *name, int channel, obs_data_t *parent,
		vector<OBSSource> &audioSources)
{
	obs_source_t *source = obs_get_output_source(channel);
	if (!source)
		return;

	audioSources.push_back(source);

	obs_data_t *data = obs_save_source(source);

	obs_data_set_obj(parent, name, data);

	obs_data_release(data);
	obs_source_release(source);
}

static obs_data_t *GenerateSaveData(obs_data_array_t *sceneOrder,
		obs_data_array_t *quickTransitionData, int transitionDuration,
		obs_data_array_t *transitions,
		OBSScene &scene, OBSSource &curProgramScene,
		obs_data_array_t *savedProjectorList)
{
	obs_data_t *saveData = obs_data_create();

	vector<OBSSource> audioSources;
	audioSources.reserve(5);

	SaveAudioDevice(DESKTOP_AUDIO_1, 1, saveData, audioSources);
	SaveAudioDevice(DESKTOP_AUDIO_2, 2, saveData, audioSources);
	SaveAudioDevice(AUX_AUDIO_1,     3, saveData, audioSources);
	SaveAudioDevice(AUX_AUDIO_2,     4, saveData, audioSources);
	SaveAudioDevice(AUX_AUDIO_3,     5, saveData, audioSources);

	auto FilterAudioSources = [&](obs_source_t *source)
	{
		return find(begin(audioSources), end(audioSources), source) ==
				end(audioSources);
	};
	using FilterAudioSources_t = decltype(FilterAudioSources);

	obs_data_array_t *sourcesArray = obs_save_sources_filtered(
			[](void *data, obs_source_t *source)
	{
		return (*static_cast<FilterAudioSources_t*>(data))(source);
	}, static_cast<void*>(&FilterAudioSources));

	obs_source_t *transition = obs_get_output_source(0);
	obs_source_t *currentScene = obs_scene_get_source(scene);
	const char   *sceneName   = obs_source_get_name(currentScene);
	const char   *programName = obs_source_get_name(curProgramScene);

	const char *sceneCollection = config_get_string(App()->GlobalConfig(),
			"Basic", "SceneCollection");

	obs_data_set_string(saveData, "current_scene", sceneName);
	obs_data_set_string(saveData, "current_program_scene", programName);
	obs_data_set_array(saveData, "scene_order", sceneOrder);
	obs_data_set_string(saveData, "name", sceneCollection);
	obs_data_set_array(saveData, "sources", sourcesArray);
	obs_data_set_array(saveData, "quick_transitions", quickTransitionData);
	obs_data_set_array(saveData, "transitions", transitions);
	obs_data_set_array(saveData, "saved_projectors", savedProjectorList);
	obs_data_array_release(sourcesArray);

	obs_data_set_string(saveData, "current_transition",
			obs_source_get_name(transition));
	obs_data_set_int(saveData, "transition_duration", transitionDuration);
	obs_source_release(transition);

	return saveData;
}

void OBSBasic::copyActionsDynamicProperties()
{
	// Themes need the QAction dynamic properties
	for (QAction *x : ui->scenesToolbar->actions()) {
		QWidget* temp = ui->scenesToolbar->widgetForAction(x);

		for (QByteArray &y : x->dynamicPropertyNames()) {
			temp->setProperty(y, x->property(y));
		}
	}

	for (QAction *x : ui->sourcesToolbar->actions()) {
		QWidget* temp = ui->sourcesToolbar->widgetForAction(x);

		for (QByteArray &y : x->dynamicPropertyNames()) {
			temp->setProperty(y, x->property(y));
		}
	}
}

void OBSBasic::UpdateVolumeControlsDecayRate()
{
	double meterDecayRate = config_get_double(basicConfig, "Audio",
			"MeterDecayRate");

	for (size_t i = 0; i < volumes.size(); i++) {
		volumes[i]->SetMeterDecayRate(meterDecayRate);
	}
}

void OBSBasic::UpdateVolumeControlsPeakMeterType()
{
	uint32_t peakMeterTypeIdx = config_get_uint(basicConfig, "Audio",
		"PeakMeterType");

	enum obs_peak_meter_type peakMeterType;
	switch (peakMeterTypeIdx) {
	case 0:
		peakMeterType = SAMPLE_PEAK_METER;
		break;
	case 1:
		peakMeterType = TRUE_PEAK_METER;
		break;
	default:
		peakMeterType = SAMPLE_PEAK_METER;
		break;
	}

	for (size_t i = 0; i < volumes.size(); i++) {
		volumes[i]->setPeakMeterType(peakMeterType);
	}
}

void OBSBasic::ClearVolumeControls()
{
	VolControl *control;

	for (size_t i = 0; i < volumes.size(); i++) {
		control = volumes[i];
		delete control;
	}

	volumes.clear();
}

obs_data_array_t *OBSBasic::SaveSceneListOrder()
{
	obs_data_array_t *sceneOrder = obs_data_array_create();

	for (int i = 0; i < ui->scenes->count(); i++) {
		obs_data_t *data = obs_data_create();
		obs_data_set_string(data, "name",
				QT_TO_UTF8(ui->scenes->item(i)->text()));
		obs_data_array_push_back(sceneOrder, data);
		obs_data_release(data);
	}

	return sceneOrder;
}

obs_data_array_t *OBSBasic::SaveProjectors()
{
	obs_data_array_t *savedProjectors = obs_data_array_create();

	auto saveProjector = [savedProjectors](OBSProjector *projector) {
		if (!projector)
			return;

		obs_data_t *data = obs_data_create();
		ProjectorType type = projector->GetProjectorType();
		switch (type) {
		case ProjectorType::Scene:
		case ProjectorType::Source: {
			obs_source_t *source = projector->GetSource();
			const char *name = obs_source_get_name(source);
			obs_data_set_string(data, "name", name);
			break;
		}
		default:
			break;
		}
		obs_data_set_int(data, "monitor", projector->GetMonitor());
		obs_data_set_int(data, "type", static_cast<int>(type));
		obs_data_set_string(data, "geometry",
				projector->saveGeometry().toBase64()
						.constData());
		obs_data_array_push_back(savedProjectors, data);
		obs_data_release(data);
	};

	for (QPointer<QWidget> &proj : projectors)
		saveProjector(static_cast<OBSProjector *>(proj.data()));

	for (QPointer<QWidget> &proj : windowProjectors)
		saveProjector(static_cast<OBSProjector *>(proj.data()));

	return savedProjectors;
}

void OBSBasic::Save(const char *file)
{
	OBSScene scene = GetCurrentScene();
	OBSSource curProgramScene = OBSGetStrongRef(programScene);
	if (!curProgramScene)
		curProgramScene = obs_scene_get_source(scene);

	obs_data_array_t *sceneOrder = SaveSceneListOrder();
	obs_data_array_t *transitions = SaveTransitions();
	obs_data_array_t *quickTrData = SaveQuickTransitions();
	obs_data_array_t *savedProjectorList = SaveProjectors();
	obs_data_t *saveData = GenerateSaveData(sceneOrder, quickTrData,
			ui->transitionDuration->value(), transitions,
			scene, curProgramScene, savedProjectorList);

	obs_data_set_bool(saveData, "preview_locked", ui->preview->Locked());
	obs_data_set_bool(saveData, "scaling_enabled",
			ui->preview->IsFixedScaling());
	obs_data_set_int(saveData, "scaling_level",
			ui->preview->GetScalingLevel());
	obs_data_set_double(saveData, "scaling_off_x",
			ui->preview->GetScrollX());
	obs_data_set_double(saveData, "scaling_off_y",
			ui->preview->GetScrollY());

	if (api) {
		obs_data_t *moduleObj = obs_data_create();
		api->on_save(moduleObj);
		obs_data_set_obj(saveData, "modules", moduleObj);
		obs_data_release(moduleObj);
	}

	if (!obs_data_save_json_safe(saveData, file, "tmp", "bak"))
		blog(LOG_ERROR, "Could not save scene data to %s", file);

	obs_data_release(saveData);
	obs_data_array_release(sceneOrder);
	obs_data_array_release(quickTrData);
	obs_data_array_release(transitions);
	obs_data_array_release(savedProjectorList);
}

void OBSBasic::DeferSaveBegin()
{
	os_atomic_inc_long(&disableSaving);
}

void OBSBasic::DeferSaveEnd()
{
	long result = os_atomic_dec_long(&disableSaving);
	if (result == 0) {
		SaveProject();
	}
}

static void LoadAudioDevice(const char *name, int channel, obs_data_t *parent)
{
	obs_data_t *data = obs_data_get_obj(parent, name);
	if (!data)
		return;

	obs_source_t *source = obs_load_source(data);
	if (source) {
		obs_set_output_source(channel, source);
		obs_source_release(source);
	}

	obs_data_release(data);
}

static inline bool HasAudioDevices(const char *source_id)
{
	const char *output_id = source_id;
	obs_properties_t *props = obs_get_source_properties(output_id);
	size_t count = 0;

	if (!props)
		return false;

	obs_property_t *devices = obs_properties_get(props, "device_id");
	if (devices)
		count = obs_property_list_item_count(devices);

	obs_properties_destroy(props);

	return count != 0;
}

void OBSBasic::CreateFirstRunSources()
{
	bool hasDesktopAudio = HasAudioDevices(App()->OutputAudioSource());
	bool hasInputAudio   = HasAudioDevices(App()->InputAudioSource());
	// 直接屏蔽电脑输出声音，彻底避免互动时的声音啸叫...
	// 2018.12.08 - by jackey => 打开电脑输出声音...
	//if (hasDesktopAudio) {
	//	ResetAudioDevice(App()->OutputAudioSource(), "default",	Str("Basic.DesktopDevice1"), 1);
	//}
	if (hasInputAudio) {
		ResetAudioDevice(App()->InputAudioSource(), "default", Str("Basic.AuxDevice1"), 3);
	}
}

void OBSBasic::CreateDefaultScene(bool firstStart)
{
	disableSaving++;

	ClearSceneData();
	InitDefaultTransitions();
	CreateDefaultQuickTransitions();
	ui->transitionDuration->setValue(300);
	SetTransition(fadeTransition);

	obs_scene_t  *scene  = obs_scene_create(Str("Basic.Scene"));

	if (firstStart) {
		CreateFirstRunSources();
	}

	AddScene(obs_scene_get_source(scene));
	SetCurrentScene(scene, true);
	obs_scene_release(scene);

	disableSaving--;
}

static void ReorderItemByName(QListWidget *lw, const char *name, int newIndex)
{
	for (int i = 0; i < lw->count(); i++) {
		QListWidgetItem *item = lw->item(i);

		if (strcmp(name, QT_TO_UTF8(item->text())) == 0) {
			if (newIndex != i) {
				item = lw->takeItem(i);
				lw->insertItem(newIndex, item);
			}
			break;
		}
	}
}

void OBSBasic::LoadSceneListOrder(obs_data_array_t *array)
{
	size_t num = obs_data_array_count(array);

	for (size_t i = 0; i < num; i++) {
		obs_data_t *data = obs_data_array_item(array, i);
		const char *name = obs_data_get_string(data, "name");

		ReorderItemByName(ui->scenes, name, (int)i);

		obs_data_release(data);
	}
}

void OBSBasic::LoadSavedProjectors(obs_data_array_t *array)
{
	size_t num = obs_data_array_count(array);

	for (size_t i = 0; i < num; i++) {
		obs_data_t *data = obs_data_array_item(array, i);

		SavedProjectorInfo *info = new SavedProjectorInfo();
		info->monitor = obs_data_get_int(data, "monitor");
		info->type = static_cast<ProjectorType>(obs_data_get_int(data,
				"type"));
		info->geometry = std::string(
				obs_data_get_string(data, "geometry"));
		info->name = std::string(obs_data_get_string(data, "name"));
		savedProjectorsArray.emplace_back(info);

		obs_data_release(data);
	}
}

static void LogFilter(obs_source_t*, obs_source_t *filter, void *v_val)
{
	const char *name = obs_source_get_name(filter);
	const char *id = obs_source_get_id(filter);
	int val = (int)(intptr_t)v_val;
	string indent;

	for (int i = 0; i < val; i++)
		indent += "    ";

	blog(LOG_INFO, "%s- filter: '%s' (%s)", indent.c_str(), name, id);
}

static bool LogSceneItem(obs_scene_t*, obs_sceneitem_t *item, void*)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	const char *name = obs_source_get_name(source);
	const char *id = obs_source_get_id(source);

	blog(LOG_INFO, "    - source: '%s' (%s)", name, id);

	obs_monitoring_type monitoring_type =
		obs_source_get_monitoring_type(source);

	if (monitoring_type != OBS_MONITORING_TYPE_NONE) {
		const char *type =
			(monitoring_type == OBS_MONITORING_TYPE_MONITOR_ONLY)
			? "monitor only"
			: "monitor and output";

		blog(LOG_INFO, "        - monitoring: %s", type);
	}

	obs_source_enum_filters(source, LogFilter, (void*)(intptr_t)2);
	return true;
}

void OBSBasic::LogScenes()
{
	blog(LOG_INFO, "------------------------------------------------");
	blog(LOG_INFO, "Loaded scenes:");

	for (int i = 0; i < ui->scenes->count(); i++) {
		QListWidgetItem *item = ui->scenes->item(i);
		OBSScene scene = GetOBSRef<OBSScene>(item);

		obs_source_t *source = obs_scene_get_source(scene);
		const char *name = obs_source_get_name(source);

		blog(LOG_INFO, "- scene '%s':", name);
		obs_scene_enum_items(scene, LogSceneItem, nullptr);
		obs_source_enum_filters(source, LogFilter, (void*)(intptr_t)1);
	}

	blog(LOG_INFO, "------------------------------------------------");
}

void OBSBasic::Load(const char *file)
{
	ProfileScope("OBSBasic::Load");

	disableSaving++;

	obs_data_t *data = obs_data_create_from_json_file_safe(file, "bak");
	if (!data) {
		disableSaving--;
		blog(LOG_INFO, "No scene file found, creating default scene");
		CreateDefaultScene(true);
		SaveProject();
		return;
	}

	ClearSceneData();
	InitDefaultTransitions();

	obs_data_t *modulesObj = obs_data_get_obj(data, "modules");
	if (api)
		api->on_preload(modulesObj);

	obs_data_array_t *sceneOrder = obs_data_get_array(data, "scene_order");
	obs_data_array_t *sources    = obs_data_get_array(data, "sources");
	obs_data_array_t *transitions= obs_data_get_array(data, "transitions");
	const char       *sceneName = obs_data_get_string(data,
			"current_scene");
	const char       *programSceneName = obs_data_get_string(data,
			"current_program_scene");
	const char       *transitionName = obs_data_get_string(data,
			"current_transition");

	if (!opt_starting_scene.empty()) {
		programSceneName = opt_starting_scene.c_str();
		if (!IsPreviewProgramMode())
			sceneName = opt_starting_scene.c_str();
	}

	int newDuration = obs_data_get_int(data, "transition_duration");
	if (!newDuration)
		newDuration = 300;

	if (!transitionName)
		transitionName = obs_source_get_name(fadeTransition);

	const char *curSceneCollection = config_get_string(
			App()->GlobalConfig(), "Basic", "SceneCollection");

	obs_data_set_default_string(data, "name", curSceneCollection);

	const char       *name = obs_data_get_string(data, "name");
	obs_source_t     *curScene;
	obs_source_t     *curProgramScene;
	obs_source_t     *curTransition;

	if (!name || !*name)
		name = curSceneCollection;

	LoadAudioDevice(DESKTOP_AUDIO_1, 1, data);
	LoadAudioDevice(DESKTOP_AUDIO_2, 2, data);
	LoadAudioDevice(AUX_AUDIO_1,     3, data);
	LoadAudioDevice(AUX_AUDIO_2,     4, data);
	LoadAudioDevice(AUX_AUDIO_3,     5, data);

	obs_load_sources(sources, OBSBasic::SourceLoaded, this);

	if (transitions)
		LoadTransitions(transitions);
	if (sceneOrder)
		LoadSceneListOrder(sceneOrder);

	obs_data_array_release(transitions);

	curTransition = FindTransition(transitionName);
	if (!curTransition)
		curTransition = fadeTransition;

	ui->transitionDuration->setValue(newDuration);
	SetTransition(curTransition);

retryScene:
	curScene = obs_get_source_by_name(sceneName);
	curProgramScene = obs_get_source_by_name(programSceneName);

	/* if the starting scene command line parameter is bad at all,
	 * fall back to original settings */
	if (!opt_starting_scene.empty() && (!curScene || !curProgramScene)) {
		sceneName = obs_data_get_string(data, "current_scene");
		programSceneName = obs_data_get_string(data,
				"current_program_scene");
		obs_source_release(curScene);
		obs_source_release(curProgramScene);
		opt_starting_scene.clear();
		goto retryScene;
	}

	if (!curProgramScene) {
		curProgramScene = curScene;
		obs_source_addref(curScene);
	}

	SetCurrentScene(curScene, true);
	if (IsPreviewProgramMode())
		TransitionToScene(curProgramScene, true);
	obs_source_release(curScene);
	obs_source_release(curProgramScene);

	obs_data_array_release(sources);
	obs_data_array_release(sceneOrder);

	/* ------------------- */

	bool projectorSave = config_get_bool(GetGlobalConfig(), "BasicWindow",
			"SaveProjectors");

	if (projectorSave) {
		obs_data_array_t *savedProjectors = obs_data_get_array(data,
				"saved_projectors");

		if (savedProjectors) {
			LoadSavedProjectors(savedProjectors);
			OpenSavedProjectors();
			activateWindow();
		}

		obs_data_array_release(savedProjectors);
	}

	/* ------------------- */

	std::string file_base = strrchr(file, '/') + 1;
	file_base.erase(file_base.size() - 5, 5);

	config_set_string(App()->GlobalConfig(), "Basic", "SceneCollection",
			name);
	config_set_string(App()->GlobalConfig(), "Basic", "SceneCollectionFile",
			file_base.c_str());

	obs_data_array_t *quickTransitionData = obs_data_get_array(data,
			"quick_transitions");
	LoadQuickTransitions(quickTransitionData);
	obs_data_array_release(quickTransitionData);

	RefreshQuickTransitions();

	// 默认强制使用预览锁定方式 => 不能让场景资源预览窗口自由变换位置...
	bool previewLocked = true; //obs_data_get_bool(data, "preview_locked");
	ui->preview->SetLocked(previewLocked);
	ui->actionLockPreview->setChecked(previewLocked);

	/* ---------------------- */

	bool fixedScaling = obs_data_get_bool(data, "scaling_enabled");
	int scalingLevel = (int)obs_data_get_int(data, "scaling_level");
	float scrollOffX = (float)obs_data_get_double(data, "scaling_off_x");
	float scrollOffY = (float)obs_data_get_double(data, "scaling_off_y");

	// 强制预览框使用锁定缩放模式 => 自动根据窗口变化自动缩放所有场景资源...
	fixedScaling = false;

	if (fixedScaling) {
		ui->preview->SetScalingLevel(scalingLevel);
		ui->preview->SetScrollingOffset(scrollOffX, scrollOffY);
	}
	ui->preview->SetFixedScaling(fixedScaling);

	/* ---------------------- */

	if (api)
		api->on_load(modulesObj);

	obs_data_release(modulesObj);
	obs_data_release(data);

	if (!opt_starting_scene.empty())
		opt_starting_scene.clear();

	if (opt_start_streaming) {
		blog(LOG_INFO, "Starting stream due to command line parameter");
		QMetaObject::invokeMethod(this, "StartStreaming",
				Qt::QueuedConnection);
		opt_start_streaming = false;
	}

	if (opt_start_recording) {
		blog(LOG_INFO, "Starting recording due to command line parameter");
		QMetaObject::invokeMethod(this, "StartRecording",
				Qt::QueuedConnection);
		opt_start_recording = false;
	}

	if (opt_start_replaybuffer) {
		QMetaObject::invokeMethod(this, "StartReplayBuffer",
				Qt::QueuedConnection);
		opt_start_replaybuffer = false;
	}

	LogScenes();

	disableSaving--;

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_SCENE_CHANGED);
}

#define SERVICE_PATH "service.json"

// 注意：现在是udp模式，这个接口用不着了...
// 将获取的云教室直播地址更新到service.json当中...
/*void OBSBasic::doUpdateService()
{
	// 必须在读取配置之前操作...
	if (service != NULL)
		return;
	// 从已经加载的全局配置当中获取直播地址...
	const char * lpLiveRoomKey = config_get_string(App()->GlobalConfig(), "General", "LiveRoomKey");
	const char * lpLiveRoomServer = config_get_string(App()->GlobalConfig(), "General", "LiveRoomServer");
	if (lpLiveRoomKey == NULL || lpLiveRoomServer == NULL)
		return;
	// 获取存盘目录地址...
	char serviceJsonPath[512];
	int ret = this->GetProfilePath(serviceJsonPath, sizeof(serviceJsonPath), SERVICE_PATH);
	if (ret <= 0)
		return;
	// 完全重新准备存盘需要的数据...
	obs_data_t * dataSave = obs_data_create();
	obs_data_t * settings = obs_data_create();
	obs_data_set_string(settings, "key", lpLiveRoomKey);
	obs_data_set_string(settings, "server", lpLiveRoomServer);
	obs_data_set_obj(dataSave, "settings", settings);
	obs_data_set_string(dataSave, "type", "rtmp_custom");
	// 调用存盘接口进行存盘操作...
	if (!obs_data_save_json_safe(dataSave, serviceJsonPath, "tmp", "bak")) {
		blog(LOG_WARNING, "Failed to save service");
	}
	// 减少引用计数，释放资源 => 注意：相互引用指针，释放顺序不受影响...
	obs_data_release(dataSave);
	obs_data_release(settings);
}*/

void OBSBasic::SaveService()
{
	if (!service)
		return;

	char serviceJsonPath[512];
	int ret = GetProfilePath(serviceJsonPath, sizeof(serviceJsonPath), SERVICE_PATH);
	if (ret <= 0)
		return;

	obs_data_t *data     = obs_data_create();
	obs_data_t *settings = obs_service_get_settings(service);

	obs_data_set_string(data, "type", obs_service_get_type(service));
	obs_data_set_obj(data, "settings", settings);

	if (!obs_data_save_json_safe(data, serviceJsonPath, "tmp", "bak"))
		blog(LOG_WARNING, "Failed to save service");

	obs_data_release(settings);
	obs_data_release(data);
}

bool OBSBasic::LoadService()
{
	const char *type;

	char serviceJsonPath[512];
	int ret = GetProfilePath(serviceJsonPath, sizeof(serviceJsonPath),
			SERVICE_PATH);
	if (ret <= 0)
		return false;

	obs_data_t *data = obs_data_create_from_json_file_safe(serviceJsonPath,
			"bak");

	obs_data_set_default_string(data, "type", "rtmp_common");
	type = obs_data_get_string(data, "type");

	obs_data_t *settings = obs_data_get_obj(data, "settings");
	obs_data_t *hotkey_data = obs_data_get_obj(data, "hotkeys");

	service = obs_service_create(type, "default_service", settings,
			hotkey_data);
	obs_service_release(service);

	obs_data_release(hotkey_data);
	obs_data_release(settings);
	obs_data_release(data);

	return !!service;
}

bool OBSBasic::InitService()
{
	ProfileScope("OBSBasic::InitService");

	if (LoadService())
		return true;

	service = obs_service_create("rtmp_common", "default_service", nullptr,
			nullptr);
	if (!service)
		return false;
	obs_service_release(service);

	return true;
}

static const double scaled_vals[] =
{
	1.0,
	1.25,
	(1.0/0.75),
	1.5,
	(1.0/0.6),
	1.75,
	2.0,
	2.25,
	2.5,
	2.75,
	3.0,
	0.0
};

bool OBSBasic::InitBasicConfigDefaults()
{
	QList<QScreen*> screens = QGuiApplication::screens();

	if (!screens.size()) {
		OBSErrorBox(NULL, "There appears to be no monitors.  Er, this "
		                  "technically shouldn't be possible.");
		return false;
	}

	QScreen *primaryScreen = QGuiApplication::primaryScreen();

	uint32_t cx = primaryScreen->size().width();
	uint32_t cy = primaryScreen->size().height();

	bool oldResolutionDefaults = config_get_bool(App()->GlobalConfig(),
			"General", "Pre19Defaults");

	/* use 1920x1080 for new default base res if main monitor is above
	 * 1920x1080, but don't apply for people from older builds -- only to
	 * new users */
	if (!oldResolutionDefaults && (cx * cy) > (1920 * 1080)) {
		cx = 1920;
		cy = 1080;
	}

	/* ----------------------------------------------------- */
	/* move over mixer values in advanced if older config */
	if (config_has_user_value(basicConfig, "AdvOut", "RecTrackIndex") &&
	    !config_has_user_value(basicConfig, "AdvOut", "RecTracks")) {

		uint64_t track = config_get_uint(basicConfig, "AdvOut",	"RecTrackIndex");
		track = 1ULL << (track - 1);
		config_set_uint(basicConfig, "AdvOut", "RecTracks", track);
		config_remove_value(basicConfig, "AdvOut", "RecTrackIndex");
		config_save_safe(basicConfig, "tmp", nullptr);
	}

	/* ----------------------------------------------------- */

	// 对外输出模式选择 => 默认选择了 Advanced => 主要包含录像配置和压缩器配置...
	config_set_default_string(basicConfig, "Output", "Mode", "Advanced"); //"Simple");

	// 这是针对 Simple 输出模式的配置 => 目前用的高级输出模式...
	config_set_default_string(basicConfig, "SimpleOutput", "FilePath", GetDefaultVideoSavePath().c_str());
	config_set_default_string(basicConfig, "SimpleOutput", "RecFormat",	"mp4");
	config_set_default_uint  (basicConfig, "SimpleOutput", "VBitrate", 1024); //2500
	config_set_default_string(basicConfig, "SimpleOutput", "StreamEncoder", SIMPLE_ENCODER_X264);
	config_set_default_uint  (basicConfig, "SimpleOutput", "ABitrate", 64); //160
	config_set_default_bool  (basicConfig, "SimpleOutput", "UseAdvanced", false);
	config_set_default_bool  (basicConfig, "SimpleOutput", "EnforceBitrate", true);
	config_set_default_string(basicConfig, "SimpleOutput", "Preset", "veryfast");
	config_set_default_string(basicConfig, "SimpleOutput", "RecQuality", "Stream");
	config_set_default_string(basicConfig, "SimpleOutput", "RecEncoder", SIMPLE_ENCODER_X264);
	config_set_default_bool(basicConfig, "SimpleOutput", "RecRB", false);
	config_set_default_int(basicConfig, "SimpleOutput", "RecRBTime", 20);
	config_set_default_int(basicConfig, "SimpleOutput", "RecRBSize", 512);
	config_set_default_string(basicConfig, "SimpleOutput", "RecRBPrefix", "Replay");

	// 这是针对 Advanced 输出模式的配置 => 包含 网络流输出 和 录像输出...
	config_set_default_bool  (basicConfig, "AdvOut", "ApplyServiceSettings", true);
	config_set_default_bool  (basicConfig, "AdvOut", "UseRescale", false);
	config_set_default_uint  (basicConfig, "AdvOut", "TrackIndex", 1); // 默认网络流和录像都用轨道1(索引编号是0)
	config_set_default_string(basicConfig, "AdvOut", "Encoder", "obs_x264");

	// Advanced 输出模式当中针对录像的类型配置 => Standard|FFmpeg => 采用Standard模式...
	config_set_default_string(basicConfig, "AdvOut", "RecType", "Standard");

	// Standard标准录像模式的参数配置 => 音频可以支持多轨道录像 => 目前使用这些配置...
	config_set_default_string(basicConfig, "AdvOut", "RecFilePath", GetDefaultVideoSavePath().c_str());
	config_set_default_string(basicConfig, "AdvOut", "RecFormat", "mp4");
	config_set_default_bool  (basicConfig, "AdvOut", "RecUseRescale", false);
	config_set_default_bool  (basicConfig, "AdvOut", "RecFileNameWithoutSpace", true); //标准录像文件名不包含空格
	config_set_default_uint  (basicConfig, "AdvOut", "RecTracks", (2<<0)); //标准录像用轨道2(索引编号是1) => 支持多音轨录像
	config_set_default_string(basicConfig, "AdvOut", "RecEncoder", "none");

	// FFmpeg自定义录像模式的参数配置 => 音频只支持1个轨道录像 => 目前没有使用...
	config_set_default_bool  (basicConfig, "AdvOut", "FFOutputToFile", true);
	config_set_default_string(basicConfig, "AdvOut", "FFFilePath", GetDefaultVideoSavePath().c_str());
	config_set_default_string(basicConfig, "AdvOut", "FFExtension", "mp4");
	config_set_default_uint  (basicConfig, "AdvOut", "FFVBitrate", 1024); //2500);
	config_set_default_uint  (basicConfig, "AdvOut", "FFVGOPSize", 150);  //250);
	config_set_default_bool  (basicConfig, "AdvOut", "FFUseRescale", false);
	config_set_default_bool  (basicConfig, "AdvOut", "FFIgnoreCompat", false);
	config_set_default_uint  (basicConfig, "AdvOut", "FFABitrate", 64); //160);
	config_set_default_uint  (basicConfig, "AdvOut", "FFAudioTrack", 1); //FFmpeg录像用轨道1 => 单音轨录像

	config_set_default_uint  (basicConfig, "AdvOut", "Track1Bitrate", 64);//160);
	config_set_default_uint  (basicConfig, "AdvOut", "Track2Bitrate", 64);//160);
	config_set_default_uint  (basicConfig, "AdvOut", "Track3Bitrate", 64);//160);
	config_set_default_uint  (basicConfig, "AdvOut", "Track4Bitrate", 64);//160);
	config_set_default_uint  (basicConfig, "AdvOut", "Track5Bitrate", 64);//160);
	config_set_default_uint  (basicConfig, "AdvOut", "Track6Bitrate", 64);//160);

	config_set_default_bool  (basicConfig, "AdvOut", "RecRB", false);
	config_set_default_uint  (basicConfig, "AdvOut", "RecRBTime", 20);
	config_set_default_int   (basicConfig, "AdvOut", "RecRBSize", 512);

	config_set_default_uint  (basicConfig, "Video", "BaseCX",   cx);
	config_set_default_uint  (basicConfig, "Video", "BaseCY",   cy);

	/* don't allow BaseCX/BaseCY to be susceptible to defaults changing */
	if (!config_has_user_value(basicConfig, "Video", "BaseCX") ||
	    !config_has_user_value(basicConfig, "Video", "BaseCY")) {
		config_set_uint(basicConfig, "Video", "BaseCX", cx);
		config_set_uint(basicConfig, "Video", "BaseCY", cy);
		config_save_safe(basicConfig, "tmp", nullptr);
	}

	config_set_default_string(basicConfig, "Output", "FilenameFormatting",
			"%CCYY-%MM-%DD %hh-%mm-%ss");

	config_set_default_bool  (basicConfig, "Output", "DelayEnable", false);
	config_set_default_uint  (basicConfig, "Output", "DelaySec", 20);
	config_set_default_bool  (basicConfig, "Output", "DelayPreserve", true);

	config_set_default_bool  (basicConfig, "Output", "Reconnect", true);
	config_set_default_uint  (basicConfig, "Output", "RetryDelay", 10);
	config_set_default_uint  (basicConfig, "Output", "MaxRetries", 20);

	config_set_default_string(basicConfig, "Output", "BindIP", "default");
	config_set_default_bool  (basicConfig, "Output", "NewSocketLoopEnable",
			false);
	config_set_default_bool  (basicConfig, "Output", "LowLatencyEnable",
			false);

	int i = 0;
	uint32_t scale_cx = cx;
	uint32_t scale_cy = cy;

	/* use a default scaled resolution that has a pixel count no higher
	 * than 1280x720 */
	while (((scale_cx * scale_cy) > (1280 * 720)) && scaled_vals[i] > 0.0) {
		double scale = scaled_vals[i++];
		scale_cx = uint32_t(double(cx) / scale);
		scale_cy = uint32_t(double(cy) / scale);
	}

	config_set_default_uint  (basicConfig, "Video", "OutputCX", scale_cx);
	config_set_default_uint  (basicConfig, "Video", "OutputCY", scale_cy);

	/* don't allow OutputCX/OutputCY to be susceptible to defaults
	 * changing */
	if (!config_has_user_value(basicConfig, "Video", "OutputCX") ||
	    !config_has_user_value(basicConfig, "Video", "OutputCY")) {
		config_set_uint(basicConfig, "Video", "OutputCX", scale_cx);
		config_set_uint(basicConfig, "Video", "OutputCY", scale_cy);
		config_save_safe(basicConfig, "tmp", nullptr);
	}

	config_set_default_uint  (basicConfig, "Video", "FPSType", 0);
	config_set_default_string(basicConfig, "Video", "FPSCommon", "30");
	config_set_default_uint  (basicConfig, "Video", "FPSInt", 30);
	config_set_default_uint  (basicConfig, "Video", "FPSNum", 30);
	config_set_default_uint  (basicConfig, "Video", "FPSDen", 1);
	config_set_default_string(basicConfig, "Video", "ScaleType", "bicubic");
	config_set_default_string(basicConfig, "Video", "ColorFormat", "NV12");
	config_set_default_string(basicConfig, "Video", "ColorSpace", "601");
	config_set_default_string(basicConfig, "Video", "ColorRange",
			"Partial");

	config_set_default_string(basicConfig, "Audio", "MonitoringDeviceId",
			"default");
	config_set_default_string(basicConfig, "Audio", "MonitoringDeviceName",
			Str("Basic.Settings.Advanced.Audio.MonitoringDevice"
				".Default"));
	config_set_default_uint  (basicConfig, "Audio", "SampleRate", 44100);
	config_set_default_string(basicConfig, "Audio", "ChannelSetup",
			"Stereo");
	config_set_default_double(basicConfig, "Audio", "MeterDecayRate",
			VOLUME_METER_DECAY_FAST);

	return true;
}

bool OBSBasic::InitBasicConfig()
{
	ProfileScope("OBSBasic::InitBasicConfig");

	char configPath[512];

	int ret = GetProfilePath(configPath, sizeof(configPath), "");
	if (ret <= 0) {
		OBSErrorBox(nullptr, "Failed to get profile path");
		return false;
	}

	if (os_mkdir(configPath) == MKDIR_ERROR) {
		OBSErrorBox(nullptr, "Failed to create profile path");
		return false;
	}

	ret = GetProfilePath(configPath, sizeof(configPath), "basic.ini");
	if (ret <= 0) {
		OBSErrorBox(nullptr, "Failed to get base.ini path");
		return false;
	}

	int code = basicConfig.Open(configPath, CONFIG_OPEN_ALWAYS);
	if (code != CONFIG_SUCCESS) {
		OBSErrorBox(NULL, "Failed to open basic.ini: %d", code);
		return false;
	}

	if (config_get_string(basicConfig, "General", "Name") == nullptr) {
		const char *curName = config_get_string(App()->GlobalConfig(),
				"Basic", "Profile");

		config_set_string(basicConfig, "General", "Name", curName);
		basicConfig.SaveSafe("tmp");
	}

	return InitBasicConfigDefaults();
}

/*extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
};

static inline enum AVPixelFormat obs_to_ffmpeg_video_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_NONE: return AV_PIX_FMT_NONE;
	case VIDEO_FORMAT_I444: return AV_PIX_FMT_YUV444P;
	case VIDEO_FORMAT_I420: return AV_PIX_FMT_YUV420P;
	case VIDEO_FORMAT_NV12: return AV_PIX_FMT_NV12;
	case VIDEO_FORMAT_YVYU: return AV_PIX_FMT_NONE;
	case VIDEO_FORMAT_YUY2: return AV_PIX_FMT_YUYV422;
	case VIDEO_FORMAT_UYVY: return AV_PIX_FMT_UYVY422;
	case VIDEO_FORMAT_RGBA: return AV_PIX_FMT_RGBA;
	case VIDEO_FORMAT_BGRA: return AV_PIX_FMT_BGRA;
	case VIDEO_FORMAT_BGRX: return AV_PIX_FMT_BGRA;
	case VIDEO_FORMAT_Y800: return AV_PIX_FMT_GRAY8;
	}
	return AV_PIX_FMT_NONE;
}

static bool DoProcSaveJpeg(struct video_data * frame)
{
	// 获取存盘需要的配置信息 => 路径和文件名...
	char szSaveFile[100] = { 0 };
	char szSavePath[300] = { 0 };
	sprintf(szSaveFile, "obs-teacher/live_%d.jpg", 200002);
	if (os_get_config_path(szSavePath, sizeof(szSavePath), szSaveFile) <= 0) {
		blog(LOG_ERROR, "DoProcSaveJpeg: save path error!");
		return false;
	}
	struct obs_video_info ovi = { 0 };
	if (!obs_get_video_info(&ovi))
		return false;
	/////////////////////////////////////////////////////////////////////////
	// 注意：input->conversion 是需要变换的格式，
	// 因此，应该从 video->info 当中获取原始数据信息...
	// 同时，sws_getContext 需要AVPixelFormat而不是video_format格式...
	/////////////////////////////////////////////////////////////////////////
	// 设置ffmpeg的日志回调函数...
	//av_log_set_level(AV_LOG_VERBOSE);
	//av_log_set_callback(my_av_logoutput);
	// 统一数据源输入格式，找到压缩器需要的像素格式 => 必须是 AV_PIX_FMT_YUVJ420P 格式...
	enum AVPixelFormat nDestFormat = AV_PIX_FMT_YUVJ420P; //AV_PIX_FMT_YUV420P
	enum AVPixelFormat nSrcFormat = obs_to_ffmpeg_video_format(ovi.output_format);
	int nSrcWidth = ovi.output_width;
	int nSrcHeight = ovi.output_height;
	// 注意：长宽必须是4的整数倍，否则sws_scale崩溃...
	int nDstWidth = nSrcWidth / 4 * 4;
	int nDstHeight = nSrcHeight / 4 * 4;
	// 不管什么格式，都需要进行像素格式的转换...
	AVFrame * pDestFrame = av_frame_alloc();
	int nDestBufSize = av_image_get_buffer_size(nDestFormat, nDstWidth, nDstHeight, 1);
	uint8_t * pDestOutBuf = (uint8_t *)av_malloc(nDestBufSize);
	av_image_fill_arrays(pDestFrame->data, pDestFrame->linesize, pDestOutBuf, nDestFormat, nDstWidth, nDstHeight, 1);

	//nv12_to_yuv420p((const uint8_t* const*)frame->data, pDestOutBuf, nDstWidth, nDstHeight);
	// 注意：这里不用libyuv的原因是，使用sws更简单，不用根据不同像素格式调用不同接口...
	// ffmpeg自带的sws_scale转换也是没有问题的，之前有问题是由于sws_getContext的输入源需要格式AVPixelFormat，写成了video_format，造成的格式错位问题...
	// 注意：目的像素格式必须为AV_PIX_FMT_YUVJ420P，如果用AV_PIX_FMT_YUV420P格式，生成的JPG有色差，而且图像偏灰色...
	struct SwsContext * img_convert_ctx = sws_getContext(nSrcWidth, nSrcHeight, nSrcFormat, nDstWidth, nDstHeight, nDestFormat, SWS_BICUBIC, NULL, NULL, NULL);
	int nReturn = sws_scale(img_convert_ctx, (const uint8_t* const*)frame->data, (const int*)frame->linesize, 0, nSrcHeight, pDestFrame->data, pDestFrame->linesize);
	sws_freeContext(img_convert_ctx);

	// 设置转换后的数据帧内容...
	pDestFrame->width = nDstWidth;
	pDestFrame->height = nDstHeight;
	pDestFrame->format = nDestFormat;

	// 将转换后的 YUV 数据存盘成 jpg 文件...
	AVCodecContext * pOutCodecCtx = NULL;
	bool bRetSave = false;
	do {
		// 预先查找jpeg压缩器需要的输入数据格式...
		AVOutputFormat * avOutputFormat = av_guess_format("mjpeg", NULL, NULL); //av_guess_format(0, lpszJpgName, 0);
		AVCodec * pOutAVCodec = avcodec_find_encoder(avOutputFormat->video_codec);
		if (pOutAVCodec == NULL)
			break;
		// 创建ffmpeg压缩器的场景对象...
		pOutCodecCtx = avcodec_alloc_context3(pOutAVCodec);
		if (pOutCodecCtx == NULL)
			break;
		// 准备数据结构需要的参数...
		pOutCodecCtx->bit_rate = 200000;
		pOutCodecCtx->width = nDstWidth;
		pOutCodecCtx->height = nDstHeight;
		// 注意：没有使用适配方式，适配出来格式有可能不是YUVJ420P，造成压缩器崩溃，因为传递的数据已经固定成YUV420P...
		// 注意：输入像素是YUV420P格式，压缩器像素是YUVJ420P格式...
		pOutCodecCtx->pix_fmt = avcodec_find_best_pix_fmt_of_list(pOutAVCodec->pix_fmts, (AVPixelFormat)-1, 1, 0); //AV_PIX_FMT_YUVJ420P;
		pOutCodecCtx->codec_id = avOutputFormat->video_codec; //AV_CODEC_ID_MJPEG;  
		pOutCodecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
		pOutCodecCtx->time_base.num = 1;
		pOutCodecCtx->time_base.den = 25;
		// 打开 ffmpeg 压缩器...
		if (avcodec_open2(pOutCodecCtx, pOutAVCodec, 0) < 0)
			break;
		// 设置图像质量，默认是0.5，修改为0.8(图片太大,0.5刚刚好)...
		pOutCodecCtx->qcompress = 0.5f;
		// 准备接收缓存，开始压缩jpg数据...
		int got_pic = 0;
		int nResult = 0;
		AVPacket pkt = { 0 };
		// 采用新的压缩函数...
		nResult = avcodec_encode_video2(pOutCodecCtx, &pkt, pDestFrame, &got_pic);
		// 解码失败或没有得到完整图像，继续解析...
		if (nResult < 0 || !got_pic)
			break;
		// 打开jpg文件句柄...
		FILE * pFile = fopen(szSavePath, "wb");
		// 打开jpg失败，注意释放资源...
		if (pFile == NULL) {
			av_packet_unref(&pkt);
			break;
		}
		// 保存到磁盘，并释放资源...
		fwrite(pkt.data, 1, pkt.size, pFile);
		av_packet_unref(&pkt);
		// 释放文件句柄，返回成功...
		fclose(pFile); pFile = NULL;
		bRetSave = true;
	} while (false);
	// 清理中间产生的对象...
	if (pOutCodecCtx != NULL) {
		avcodec_close(pOutCodecCtx);
		av_free(pOutCodecCtx);
	}

	// 释放临时分配的数据空间...
	av_frame_free(&pDestFrame);
	av_free(pDestOutBuf);

	return bRetSave;
}

static void receive_raw_video(void *param, struct video_data *frame)
{
	OBSBasic *main = reinterpret_cast<OBSBasic*>(param);
	DoProcSaveJpeg(frame);
}*/

void OBSBasic::InitOBSCallbacks()
{
	ProfileScope("OBSBasic::InitOBSCallbacks");

	signalHandlers.reserve(signalHandlers.size() + 6);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_remove",
			OBSBasic::SourceRemoved, this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_activate",
			OBSBasic::SourceActivated, this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_deactivate",
			OBSBasic::SourceDeactivated, this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_rename",
			OBSBasic::SourceRenamed, this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_monitoring",
			OBSBasic::SourceMonitoring, this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_updated",
			OBSBasic::SourceUpdated, this);

	//obs_add_raw_video_callback(NULL, receive_raw_video, this);
}

void OBSBasic::InitPrimitives()
{
	ProfileScope("OBSBasic::InitPrimitives");

	obs_enter_graphics();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(0.0f, 0.0f);
	box = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	boxLeft = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(1.0f, 0.0f);
	boxTop = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(1.0f, 1.0f);
	boxRight = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	boxBottom = gs_render_save();

	gs_render_start(true);
	for (int i = 0; i <= 360; i += (360/20)) {
		float pos = RAD(float(i));
		gs_vertex2f(cosf(pos), sinf(pos));
	}
	circle = gs_render_save();

	obs_leave_graphics();
}

void OBSBasic::ReplayBufferClicked()
{
	if (outputHandler->ReplayBufferActive())
		StopReplayBuffer();
	else
		StartReplayBuffer();
};

void OBSBasic::ResetOutputs()
{
	ProfileScope("OBSBasic::ResetOutputs");

	const char *mode = config_get_string(basicConfig, "Output", "Mode");
	bool advOut = astrcmpi(mode, "Advanced") == 0;

	if (!outputHandler || !outputHandler->Active()) {
		outputHandler.reset();
		outputHandler.reset(advOut ?
			CreateAdvancedOutputHandler(this) :
			CreateSimpleOutputHandler(this));

		delete replayBufferButton;

		if (outputHandler->replayBuffer) {
			replayBufferButton = new QPushButton(
					QTStr("Basic.Main.StartReplayBuffer"),
					this);
			connect(replayBufferButton.data(),
					&QPushButton::clicked,
					this,
					&OBSBasic::ReplayBufferClicked);

			replayBufferButton->setProperty("themeID", "replayBufferButton");
			//ui->buttonsVLayout->insertWidget(2, replayBufferButton);
		}

		if (sysTrayReplayBuffer)
			sysTrayReplayBuffer->setEnabled(
					!!outputHandler->replayBuffer);
	} else {
		outputHandler->Update();
	}
}

static void AddProjectorMenuMonitors(QMenu *parent, QObject *target,
		const char *slot);

#define STARTUP_SEPARATOR \
	"==== Startup complete ==============================================="
#define SHUTDOWN_SEPARATOR \
	"==== Shutting down =================================================="

extern obs_frontend_callbacks *InitializeAPIInterface(OBSBasic *main);

#define UNSUPPORTED_ERROR \
	"Failed to initialize video:\n\nRequired graphics API functionality " \
	"not found.  Your GPU may not be supported."

#define UNKNOWN_ERROR \
	"Failed to initialize video.  Your GPU may not be supported, " \
	"or your graphics drivers may need to be updated."

#define OBS_D3D_INSTALL -1000

// 返回一个特殊错误号，避免发送异常...
int OBSBasic::doD3DSetup()
{
	AutoUpdateThread::doLaunchDXWebSetup();
	return OBS_D3D_INSTALL;
}

void OBSBasic::OBSInit()
{
	ProfileScope("OBSBasic::OBSInit");

	const char *sceneCollection = config_get_string(App()->GlobalConfig(),
			"Basic", "SceneCollectionFile");
	char savePath[512];
	char fileName[512];
	int ret;

	if (!sceneCollection)
		throw "Failed to get scene collection name";

	ret = snprintf(fileName, 512, "obs-teacher/basic/scenes/%s.json",
			sceneCollection);
	if (ret <= 0)
		throw "Failed to create scene collection file name";

	ret = GetConfigPath(savePath, sizeof(savePath), fileName);
	if (ret <= 0)
		throw "Failed to get scene collection json file path";

	if (!InitBasicConfig())
		throw "Failed to load basic.ini";
	if (!ResetAudio())
		throw "Failed to initialize audio";

	ret = ResetVideo();

	// 针对D3D做特殊拦截操作...
	if (ret == OBS_D3D_INSTALL)
		return;

	switch (ret) {
	case OBS_VIDEO_MODULE_NOT_FOUND:
		throw "Failed to initialize video:  Graphics module not found";
	case OBS_VIDEO_NOT_SUPPORTED:
		throw UNSUPPORTED_ERROR;
	case OBS_VIDEO_INVALID_PARAM:
		throw "Failed to initialize video:  Invalid parameters";
	default:
		if (ret != OBS_VIDEO_SUCCESS)
			throw UNKNOWN_ERROR;
	}

	/* load audio monitoring */
#if defined(_WIN32) || defined(__APPLE__) || HAVE_PULSEAUDIO
	const char *device_name = config_get_string(basicConfig, "Audio",
			"MonitoringDeviceName");
	const char *device_id = config_get_string(basicConfig, "Audio",
			"MonitoringDeviceId");

	obs_set_audio_monitoring_device(device_name, device_id);

	blog(LOG_INFO, "Audio monitoring device:\n\tname: %s\n\tid: %s",
			device_name, device_id);
#endif

	InitOBSCallbacks();
	InitHotkeys();

	api = InitializeAPIInterface(this);

	AddExtraModulePaths();
	blog(LOG_INFO, "---------------------------------");
	obs_load_all_modules();
	blog(LOG_INFO, "---------------------------------");
	obs_log_loaded_modules();
	blog(LOG_INFO, "---------------------------------");
	obs_post_load_modules();

	CheckForSimpleModeX264Fallback();

	blog(LOG_INFO, STARTUP_SEPARATOR);

	ResetOutputs();
	CreateHotkeys();

	if (!InitService()) {
		throw "Failed to initialize service";
	}

	InitPrimitives();

	sceneDuplicationMode = config_get_bool(App()->GlobalConfig(),
				"BasicWindow", "SceneDuplicationMode");
	swapScenesMode = config_get_bool(App()->GlobalConfig(),
				"BasicWindow", "SwapScenesMode");
	editPropertiesMode = config_get_bool(App()->GlobalConfig(),
				"BasicWindow", "EditPropertiesMode");

	if (!opt_studio_mode) {
		SetPreviewProgramMode(config_get_bool(App()->GlobalConfig(),
					"BasicWindow", "PreviewProgramMode"));
	} else {
		SetPreviewProgramMode(true);
		opt_studio_mode = false;
	}

#define SET_VISIBILITY(name, control) \
	do { \
		if (config_has_user_value(App()->GlobalConfig(), \
					"BasicWindow", name)) { \
			bool visible = config_get_bool(App()->GlobalConfig(), \
					"BasicWindow", name); \
			ui->control->setChecked(visible); \
		} \
	} while (false)

	SET_VISIBILITY("ShowListboxToolbars", toggleListboxToolbars);
	SET_VISIBILITY("ShowStatusBar", toggleStatusBar);
#undef SET_VISIBILITY

	// 注意：这里弹出的确认框容易被Load()过程关闭...
	// 注意：放到完全加载之后调用 => DeferredLoad()...
	//TimedCheckForUpdates();

	previewEnabled = config_get_bool(App()->GlobalConfig(),
			"BasicWindow", "PreviewEnabled");

	if (!previewEnabled && !IsPreviewProgramMode())
		QMetaObject::invokeMethod(this, "EnablePreviewDisplay",
				Qt::QueuedConnection,
				Q_ARG(bool, previewEnabled));

#ifdef _WIN32
	uint32_t winVer = GetWindowsVersion();
	if (winVer > 0 && winVer < 0x602) {
		bool disableAero = config_get_bool(basicConfig, "Video",
				"DisableAero");
		SetAeroEnabled(!disableAero);
	}
#endif

	RefreshSceneCollections();
	RefreshProfiles();
	disableSaving--;

	auto addDisplay = [this] (OBSQTDisplay *window)
	{
		obs_display_add_draw_callback(window->GetDisplay(),
				OBSBasic::RenderMain, this);

		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi))
			ResizePreview(ovi.base_width, ovi.base_height);
	};

	connect(ui->preview, &OBSQTDisplay::DisplayCreated, addDisplay);

#ifdef _WIN32
	SetWin32DropStyle(this);
	show();
#endif

	bool alwaysOnTop = config_get_bool(App()->GlobalConfig(), "BasicWindow",
			"AlwaysOnTop");
	if (alwaysOnTop || opt_always_on_top) {
		SetAlwaysOnTop(this, true);
		ui->actionAlwaysOnTop->setChecked(true);
	}

#ifndef _WIN32
	show();
#endif

	const char *dockStateStr = config_get_string(App()->GlobalConfig(),
			"BasicWindow", "DockState");
	if (!dockStateStr) {
		on_resetUI_triggered();
	} else {
		QByteArray dockState =
			QByteArray::fromBase64(QByteArray(dockStateStr));
		if (!restoreState(dockState))
			on_resetUI_triggered();
	}

	// 强制锁定所有的停靠窗...
	config_set_default_bool(App()->GlobalConfig(), "BasicWindow",
			"DocksLocked", true);
	bool docksLocked = config_get_bool(App()->GlobalConfig(),
			"BasicWindow", "DocksLocked");
	on_lockUI_toggled(docksLocked);
	// 将锁定状态更新到菜单当中...
	ui->lockUI->blockSignals(true);
	ui->lockUI->setChecked(docksLocked);
	ui->lockUI->blockSignals(false);

	// 强制隐藏场景和变换...
	ui->scenesDock->setVisible(false);
	ui->transitionsDock->setVisible(false);
	// 强制设置来源和混音停靠窗的大小...
	ui->sourcesDock->setMinimumSize(300, 200);
	ui->mixerDock->setMinimumSize(300, 200);
	ui->controlsDock->setMinimumSize(300, 100);
	// 设置选中记录背景色，避免预览框里选中时看不清的问题...
	ui->sources->setStyleSheet("QListView::item:selected {background: #4FC3F7;}");
	// 设置开始推流背景色，增加按钮高度...
	ui->streamButton->setStyleSheet("QPushButton{background-color: #FFA500; height: 35px; font-size: 20px; color: black;}"
									"QPushButton:hover{background-color:#FF8C00; color: #E0FFFF;}"
									"QPushButton:checked{color: #1E90FF;}");
	ui->streamButton->setCursor(QCursor(Qt::PointingHandCursor));
	// 设置开始录像背景色，增加按钮高度...
	ui->recordButton->setStyleSheet("QPushButton{background-color: #FF6600; height: 30px; font-size: 18px; color: black;}"
									"QPushButton:hover{background-color:#FF8C00; color: #E0FFFF;}"
									"QPushButton:checked{color: #1E90FF;}");
	ui->recordButton->setCursor(QCursor(Qt::PointingHandCursor));
	// 设置退出按钮、设置按钮、状态按钮的字体、高度、鼠标指针...
	ui->exitButton->setStyleSheet("QPushButton{height: 20px; font-size: 14px;}");
	ui->exitButton->setCursor(QCursor(Qt::PointingHandCursor));
	ui->settingsButton->setStyleSheet("QPushButton{height: 20px; font-size: 14px;}");
	ui->settingsButton->setCursor(QCursor(Qt::PointingHandCursor));
	ui->statsButton->setStyleSheet("QPushButton{height: 20px; font-size: 14px;}");
	ui->statsButton->setCursor(QCursor(Qt::PointingHandCursor));
	// 强制禁用一些用不到的数据源，避免干扰，混乱...
	obs_enable_source_type("game_capture", false);
	obs_enable_source_type("wasapi_output_capture", false);

	SystemTray(true);

	if (windowState().testFlag(Qt::WindowFullScreen))
		fullscreenInterface = true;

	bool has_last_version = config_has_user_value(App()->GlobalConfig(), "General", "LastVersion");
	bool first_run = config_get_bool(App()->GlobalConfig(), "General", "FirstRun");

	if (!first_run) {
		config_set_bool(App()->GlobalConfig(), "General", "FirstRun", true);
		config_save_safe(App()->GlobalConfig(), "tmp", nullptr);
	}

	// 去掉自动向导配置功能，全部采用手动配置...
	/*if (!first_run && !has_last_version && !Active()) {
		QString msg;
		msg = QTStr("Basic.FirstStartup.RunWizard");
		msg += "\n\n";
		msg += QTStr("Basic.FirstStartup.RunWizard.BetaWarning");

		QMessageBox::StandardButton button =
			OBSMessageBox::question(this, QTStr("Basic.AutoConfig"),
					msg);

		if (button == QMessageBox::Yes) {
			on_autoConfigure_triggered();
		} else {
			msg = QTStr("Basic.FirstStartup.RunWizard.NoClicked");
			OBSMessageBox::information(this,
					QTStr("Basic.AutoConfig"), msg);
		}
	}*/

	if (config_get_bool(basicConfig, "General", "OpenStatsOnStartup")) {
		on_stats_triggered();
	}

	OBSBasicStats::InitializeValues();

	/* ----------------------- */
	/* Add multiview menu      */

	/*ui->viewMenu->addSeparator();

	multiviewProjectorMenu = new QMenu(QTStr("MultiviewProjector"));
	ui->viewMenu->addMenu(multiviewProjectorMenu);
	connect(ui->viewMenu->menuAction(), &QAction::hovered, this,
			&OBSBasic::UpdateMultiviewProjectorMenu);
	ui->viewMenu->addAction(QTStr("MultiviewWindowed"),
			this, SLOT(OpenMultiviewWindow()));*/

#if !defined(_WIN32) && !defined(__APPLE__)
	delete ui->actionShowCrashLogs;
	delete ui->actionUploadLastCrashLog;
	delete ui->menuCrashLogs;
	delete ui->actionCheckForUpdates;
	ui->actionShowCrashLogs = nullptr;
	ui->actionUploadLastCrashLog = nullptr;
	ui->menuCrashLogs = nullptr;
	ui->actionCheckForUpdates = nullptr;
#endif

	/* This is an incredibly unpleasant hack for macOS to isolate CEF
	 * initialization until after all tasks related to Qt startup and main
	 * window initialization have completed.  There is a macOS-specific bug
	 * within either CEF and/or Qt that can cause a crash if both Qt and
	 * CEF are loading at the same time.
	 *
	 * CEF will typically load fine after about two iterations from this
	 * point, and all Qt tasks are typically fully completed after about
	 * four or five iterations, but to be "ultra" safe, an arbitrarily
	 * large number such as 10 is used.  This hack is extremely unpleasant,
	 * but is worth doing instead of being forced to isolate the entire
	 * browser plugin in to a separate process as before.
	 *
	 * Again, this hack is specific to macOS only.  Fortunately, on other
	 * operating systems, such issues do not occur. */
	QMetaObject::invokeMethod(this, "DeferredLoad",
			Qt::QueuedConnection,
			Q_ARG(QString, QT_UTF8(savePath)),
			Q_ARG(int, 1));
}

void OBSBasic::DeferredLoad(const QString &file, int requeueCount)
{
	if (--requeueCount > 0) {
		QMetaObject::invokeMethod(this, "DeferredLoad",
				Qt::QueuedConnection,
				Q_ARG(QString, file),
				Q_ARG(int, requeueCount));
		return;
	}
	// 加载系统各种配置参数...
	this->Load(QT_TO_UTF8(file));
	// 设置已加载完毕的标志...
	m_bIsLoaded = true;
	// 立即启动远程连接...
	App()->doCheckRemote();
	// 为了避免弹框被强制关闭，放在这里弹出更新确认框...
	// 2019.06.27 - by jackey => 转移到登录界面当中...
	//this->TimedCheckForUpdates();
	// 绑定左右翻页按钮的点击事件...
	ui->preview->BindBtnClickEvent();
	// 判断是否显示箭头 => 第一次保存0点数据源...
	this->doCheckBtnPage(true);
	// 为所有的互动学生端数据源创建第三方麦克风按钮...
	this->doBuildAllStudentBtnMic();
	// 这里还需补充创建监视器，有可能source在重建时，scene还没有创建...
	this->doSceneCreateMonitor();
}

// 当source的监视发生变化，需要重建场景的监视器，播放轨道3音频...
void OBSBasic::MonitoringSourceChanged(OBSSource source)
{
	//this->doSceneDestoryMonitor();
	//this->doSceneCreateMonitor();
}

void OBSBasic::doSceneCreateMonitor()
{
	OBSScene theCurScene = this->GetCurrentScene();
	bool bResult = obs_scene_create_monitor(theCurScene);
	blog(LOG_INFO, "== Scene Create Monitor result: %d ==", bResult);
}

void OBSBasic::doSceneDestoryMonitor()
{
	OBSScene theCurScene = this->GetCurrentScene();
	bool bResult = obs_scene_destory_monitor(theCurScene);
	blog(LOG_INFO, "== Scene Destory Monitor result: %d ==", bResult);
}

void OBSBasic::UpdateMultiviewProjectorMenu()
{
	/*multiviewProjectorMenu->clear();
	AddProjectorMenuMonitors(multiviewProjectorMenu, this,
			SLOT(OpenMultiviewProjector()));*/
}

void OBSBasic::InitHotkeys()
{
	ProfileScope("OBSBasic::InitHotkeys");

	struct obs_hotkeys_translations t = {};
	t.insert                       = Str("Hotkeys.Insert");
	t.del                          = Str("Hotkeys.Delete");
	t.home                         = Str("Hotkeys.Home");
	t.end                          = Str("Hotkeys.End");
	t.page_up                      = Str("Hotkeys.PageUp");
	t.page_down                    = Str("Hotkeys.PageDown");
	t.num_lock                     = Str("Hotkeys.NumLock");
	t.scroll_lock                  = Str("Hotkeys.ScrollLock");
	t.caps_lock                    = Str("Hotkeys.CapsLock");
	t.backspace                    = Str("Hotkeys.Backspace");
	t.tab                          = Str("Hotkeys.Tab");
	t.print                        = Str("Hotkeys.Print");
	t.pause                        = Str("Hotkeys.Pause");
	t.left                         = Str("Hotkeys.Left");
	t.right                        = Str("Hotkeys.Right");
	t.up                           = Str("Hotkeys.Up");
	t.down                         = Str("Hotkeys.Down");
#ifdef _WIN32
	t.meta                         = Str("Hotkeys.Windows");
#else
	t.meta                         = Str("Hotkeys.Super");
#endif
	t.menu                         = Str("Hotkeys.Menu");
	t.space                        = Str("Hotkeys.Space");
	t.numpad_num                   = Str("Hotkeys.NumpadNum");
	t.numpad_multiply              = Str("Hotkeys.NumpadMultiply");
	t.numpad_divide                = Str("Hotkeys.NumpadDivide");
	t.numpad_plus                  = Str("Hotkeys.NumpadAdd");
	t.numpad_minus                 = Str("Hotkeys.NumpadSubtract");
	t.numpad_decimal               = Str("Hotkeys.NumpadDecimal");
	t.apple_keypad_num             = Str("Hotkeys.AppleKeypadNum");
	t.apple_keypad_multiply        = Str("Hotkeys.AppleKeypadMultiply");
	t.apple_keypad_divide          = Str("Hotkeys.AppleKeypadDivide");
	t.apple_keypad_plus            = Str("Hotkeys.AppleKeypadAdd");
	t.apple_keypad_minus           = Str("Hotkeys.AppleKeypadSubtract");
	t.apple_keypad_decimal         = Str("Hotkeys.AppleKeypadDecimal");
	t.apple_keypad_equal           = Str("Hotkeys.AppleKeypadEqual");
	t.mouse_num                    = Str("Hotkeys.MouseButton");
	obs_hotkeys_set_translations(&t);

	obs_hotkeys_set_audio_hotkeys_translations(Str("Mute"), Str("Unmute"),
			Str("Push-to-mute"), Str("Push-to-talk"));

	obs_hotkeys_set_sceneitem_hotkeys_translations(
			Str("SceneItemShow"), Str("SceneItemHide"));

	obs_hotkey_enable_callback_rerouting(true);
	obs_hotkey_set_callback_routing_func(OBSBasic::HotkeyTriggered, this);
}

void OBSBasic::ProcessHotkey(obs_hotkey_id id, bool pressed)
{
	obs_hotkey_trigger_routed_callback(id, pressed);
}

void OBSBasic::HotkeyTriggered(void *data, obs_hotkey_id id, bool pressed)
{
	OBSBasic &basic = *static_cast<OBSBasic*>(data);
	QMetaObject::invokeMethod(&basic, "ProcessHotkey",
			Q_ARG(obs_hotkey_id, id), Q_ARG(bool, pressed));
}

void OBSBasic::CreateHotkeys()
{
	ProfileScope("OBSBasic::CreateHotkeys");

	auto LoadHotkeyData = [&](const char *name) -> OBSData
	{
		const char *info = config_get_string(basicConfig,
				"Hotkeys", name);
		if (!info)
			return {};

		obs_data_t *data = obs_data_create_from_json(info);
		if (!data)
			return {};

		OBSData res = data;
		obs_data_release(data);
		return res;
	};

	auto LoadHotkey = [&](obs_hotkey_id id, const char *name)
	{
		obs_data_array_t *array =
			obs_data_get_array(LoadHotkeyData(name), "bindings");

		obs_hotkey_load(id, array);
		obs_data_array_release(array);
	};

	auto LoadHotkeyPair = [&](obs_hotkey_pair_id id, const char *name0,
			const char *name1)
	{
		obs_data_array_t *array0 =
			obs_data_get_array(LoadHotkeyData(name0), "bindings");
		obs_data_array_t *array1 =
			obs_data_get_array(LoadHotkeyData(name1), "bindings");

		obs_hotkey_pair_load(id, array0, array1);
		obs_data_array_release(array0);
		obs_data_array_release(array1);
	};

#define MAKE_CALLBACK(pred, method, log_action) \
	[](void *data, obs_hotkey_pair_id, obs_hotkey_t*, bool pressed) \
	{ \
		OBSBasic &basic = *static_cast<OBSBasic*>(data); \
		if (pred && pressed) { \
			blog(LOG_INFO, log_action " due to hotkey"); \
			method(); \
			return true; \
		} \
		return false; \
	}

	streamingHotkeys = obs_hotkey_pair_register_frontend(
			"OBSBasic.StartStreaming",
			Str("Basic.Main.StartStreaming"),
			"OBSBasic.StopStreaming",
			Str("Basic.Main.StopStreaming"),
			MAKE_CALLBACK(!basic.outputHandler->StreamingActive(),
				basic.StartStreaming, "Starting stream"),
			MAKE_CALLBACK(basic.outputHandler->StreamingActive(),
				basic.StopStreaming, "Stopping stream"),
			this, this);
	LoadHotkeyPair(streamingHotkeys,
			"OBSBasic.StartStreaming", "OBSBasic.StopStreaming");

	auto cb = [] (void *data, obs_hotkey_id, obs_hotkey_t*, bool pressed)
	{
		OBSBasic &basic = *static_cast<OBSBasic*>(data);
		if (basic.outputHandler->StreamingActive() && pressed) {
			basic.ForceStopStreaming();
		}
	};

	forceStreamingStopHotkey = obs_hotkey_register_frontend(
			"OBSBasic.ForceStopStreaming",
			Str("Basic.Main.ForceStopStreaming"),
			cb, this);
	LoadHotkey(forceStreamingStopHotkey,
			"OBSBasic.ForceStopStreaming");

	recordingHotkeys = obs_hotkey_pair_register_frontend(
			"OBSBasic.StartRecording",
			Str("Basic.Main.StartRecording"),
			"OBSBasic.StopRecording",
			Str("Basic.Main.StopRecording"),
			MAKE_CALLBACK(!basic.outputHandler->RecordingActive(),
				basic.StartRecording, "Starting recording"),
			MAKE_CALLBACK(basic.outputHandler->RecordingActive(),
				basic.StopRecording, "Stopping recording"),
			this, this);
	LoadHotkeyPair(recordingHotkeys,
			"OBSBasic.StartRecording", "OBSBasic.StopRecording");

	replayBufHotkeys = obs_hotkey_pair_register_frontend(
			"OBSBasic.StartReplayBuffer",
			Str("Basic.Main.StartReplayBuffer"),
			"OBSBasic.StopReplayBuffer",
			Str("Basic.Main.StopReplayBuffer"),
			MAKE_CALLBACK(!basic.outputHandler->ReplayBufferActive(),
				basic.StartReplayBuffer, "Starting replay buffer"),
			MAKE_CALLBACK(basic.outputHandler->ReplayBufferActive(),
				basic.StopReplayBuffer, "Stopping replay buffer"),
			this, this);
	LoadHotkeyPair(replayBufHotkeys,
			"OBSBasic.StartReplayBuffer", "OBSBasic.StopReplayBuffer");
#undef MAKE_CALLBACK

	auto togglePreviewProgram = [] (void *data, obs_hotkey_id,
			obs_hotkey_t*, bool pressed)
	{
		if (pressed)
			QMetaObject::invokeMethod(static_cast<OBSBasic*>(data),
					"on_modeSwitch_clicked",
					Qt::QueuedConnection);
	};

	togglePreviewProgramHotkey = obs_hotkey_register_frontend(
			"OBSBasic.TogglePreviewProgram",
			Str("Basic.TogglePreviewProgramMode"),
			togglePreviewProgram, this);
	LoadHotkey(togglePreviewProgramHotkey, "OBSBasic.TogglePreviewProgram");

	auto transition = [] (void *data, obs_hotkey_id, obs_hotkey_t*,
			bool pressed)
	{
		if (pressed)
			QMetaObject::invokeMethod(static_cast<OBSBasic*>(data),
					"TransitionClicked",
					Qt::QueuedConnection);
	};

	transitionHotkey = obs_hotkey_register_frontend(
			"OBSBasic.Transition",
			Str("Transition"), transition, this);
	LoadHotkey(transitionHotkey, "OBSBasic.Transition");
}

void OBSBasic::ClearHotkeys()
{
	obs_hotkey_pair_unregister(streamingHotkeys);
	obs_hotkey_pair_unregister(recordingHotkeys);
	obs_hotkey_pair_unregister(replayBufHotkeys);
	obs_hotkey_unregister(forceStreamingStopHotkey);
	obs_hotkey_unregister(togglePreviewProgramHotkey);
	obs_hotkey_unregister(transitionHotkey);
}

OBSBasic::~OBSBasic()
{
	if (updateCheckThread && updateCheckThread->isRunning()) {
		updateCheckThread->wait();
	}
	if (updateCheckThread) {
		delete updateCheckThread;
		updateCheckThread = NULL;
	}
	if (logUploadThread && logUploadThread->isRunning()) {
		logUploadThread->wait();
	}
	if (logUploadThread) {
		delete logUploadThread;
		logUploadThread = NULL;
	}
	if (programOptions) {
		delete programOptions;
		programOptions = NULL;
	}
	if (program) {
		delete program;
		program = NULL;
	}

	/* XXX: any obs data must be released before calling obs_shutdown.
	 * currently, we can't automate this with C++ RAII because of the
	 * delicate nature of obs_shutdown needing to be freed before the UI
	 * can be freed, and we have no control over the destruction order of
	 * the Qt UI stuff, so we have to manually clear any references to
	 * libobs. */
	if (cpuUsageTimer) {
		delete cpuUsageTimer;
		cpuUsageTimer = NULL;
	}
	os_cpu_usage_info_destroy(cpuUsageInfo);

	obs_hotkey_set_callback_routing_func(nullptr, nullptr);
	ClearHotkeys();

	service = nullptr;
	outputHandler.reset();

	if (interaction)
		delete interaction;

	if (properties)
		delete properties;

	if (transformWindow)
		delete transformWindow;

	if (filters)
		delete filters;

	if (advAudioWindow)
		delete advAudioWindow;

	obs_display_remove_draw_callback(ui->preview->GetDisplay(),
			OBSBasic::RenderMain, this);

	//obs_remove_raw_video_callback(receive_raw_video, this);

	obs_enter_graphics();
	gs_vertexbuffer_destroy(box);
	gs_vertexbuffer_destroy(boxLeft);
	gs_vertexbuffer_destroy(boxTop);
	gs_vertexbuffer_destroy(boxRight);
	gs_vertexbuffer_destroy(boxBottom);
	gs_vertexbuffer_destroy(circle);
	obs_leave_graphics();

	/* When shutting down, sometimes source references can get in to the
	 * event queue, and if we don't forcibly process those events they
	 * won't get processed until after obs_shutdown has been called.  I
	 * really wish there were a more elegant way to deal with this via C++,
	 * but Qt doesn't use C++ in a normal way, so you can't really rely on
	 * normal C++ behavior for your data to be freed in the order that you
	 * expect or want it to. */
	QApplication::sendPostedEvents(this);

	config_set_int(App()->GlobalConfig(), "General", "LastVersion", LIBOBS_API_VER);

	bool alwaysOnTop = IsAlwaysOnTop(this);

	config_set_bool(App()->GlobalConfig(), "BasicWindow", "PreviewEnabled", previewEnabled);
	config_set_bool(App()->GlobalConfig(), "BasicWindow", "AlwaysOnTop", alwaysOnTop);
	config_set_bool(App()->GlobalConfig(), "BasicWindow", "SceneDuplicationMode", sceneDuplicationMode);
	config_set_bool(App()->GlobalConfig(), "BasicWindow", "SwapScenesMode", swapScenesMode);
	config_set_bool(App()->GlobalConfig(), "BasicWindow", "EditPropertiesMode", editPropertiesMode);
	config_set_bool(App()->GlobalConfig(), "BasicWindow", "PreviewProgramMode", IsPreviewProgramMode());
	config_set_bool(App()->GlobalConfig(), "BasicWindow", "DocksLocked", ui->lockUI->isChecked());
	config_save_safe(App()->GlobalConfig(), "tmp", nullptr);

#ifdef _WIN32
	uint32_t winVer = GetWindowsVersion();
	if (winVer > 0 && winVer < 0x602) {
		bool disableAero = config_get_bool(basicConfig, "Video", "DisableAero");
		if (disableAero) {
			SetAeroEnabled(true);
		}
	}
#endif
}

void OBSBasic::SaveProjectNow()
{
	if (disableSaving)
		return;

	projectChanged = true;
	SaveProjectDeferred();
}

void OBSBasic::SaveProject()
{
	if (disableSaving)
		return;

	projectChanged = true;
	QMetaObject::invokeMethod(this, "SaveProjectDeferred", Qt::QueuedConnection);
}

void OBSBasic::SaveProjectDeferred()
{
	if (disableSaving)
		return;

	if (!projectChanged)
		return;

	projectChanged = false;

	const char *sceneCollection = config_get_string(App()->GlobalConfig(), "Basic", "SceneCollectionFile");
	char savePath[512];
	char fileName[512];
	int ret;

	if (!sceneCollection)
		return;

	ret = snprintf(fileName, 512, "obs-teacher/basic/scenes/%s.json", sceneCollection);
	if (ret <= 0)
		return;

	ret = GetConfigPath(savePath, sizeof(savePath), fileName);
	if (ret <= 0)
		return;

	Save(savePath);
}

OBSSource OBSBasic::GetProgramSource()
{
	return OBSGetStrongRef(programScene);
}

OBSScene OBSBasic::GetCurrentScene()
{
	QListWidgetItem *item = ui->scenes->currentItem();
	return item ? GetOBSRef<OBSScene>(item) : nullptr;
}

OBSSceneItem OBSBasic::GetSceneItem(QListWidgetItem *item)
{
	return item ? GetOBSRef<OBSSceneItem>(item) : nullptr;
}

OBSSceneItem OBSBasic::GetCurrentSceneItem()
{
	return GetSceneItem(GetTopSelectedSourceItem());
}

void OBSBasic::UpdatePreviewScalingMenu()
{
	bool fixedScaling = ui->preview->IsFixedScaling();
	float scalingAmount = ui->preview->GetScalingAmount();
	if (!fixedScaling) {
		ui->actionScaleWindow->setChecked(true);
		ui->actionScaleCanvas->setChecked(false);
		ui->actionScaleOutput->setChecked(false);
		return;
	}

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	ui->actionScaleWindow->setChecked(false);
	ui->actionScaleCanvas->setChecked(scalingAmount == 1.0f);
	ui->actionScaleOutput->setChecked(
			scalingAmount == float(ovi.output_width) / float(ovi.base_width));
}

void OBSBasic::UpdateSources(OBSScene scene)
{
	ClearListItems(ui->sources);

	obs_scene_enum_items(scene,
			[] (obs_scene_t *scene, obs_sceneitem_t *item, void *p)
			{
				OBSBasic *window = static_cast<OBSBasic*>(p);
				window->InsertSceneItem(item);

				UNUSED_PARAMETER(scene);
				return true;
			}, this);
}

void OBSBasic::InsertSceneItem(obs_sceneitem_t *item)
{
	QListWidgetItem *listItem = new QListWidgetItem();
	SetOBSRef(listItem, OBSSceneItem(item));

	ui->sources->insertItem(0, listItem);
	ui->sources->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);

	SetupVisibilityItem(ui->sources, listItem, item);
}

void OBSBasic::CreateInteractionWindow(obs_source_t *source)
{
	if (interaction)
		interaction->close();

	interaction = new OBSBasicInteraction(this, source);
	interaction->Init();
	interaction->setAttribute(Qt::WA_DeleteOnClose, true);
}

void OBSBasic::CreatePropertiesWindow(obs_source_t *source)
{
	if (properties)
		properties->close();

	properties = new OBSBasicProperties(this, source);
	properties->Init();
	properties->setAttribute(Qt::WA_DeleteOnClose, true);
}

void OBSBasic::CreateFiltersWindow(obs_source_t *source)
{
	if (filters)
		filters->close();

	filters = new OBSBasicFilters(this, source);
	filters->Init();
	filters->setAttribute(Qt::WA_DeleteOnClose, true);
}

/* Qt callbacks for invokeMethod */

void OBSBasic::AddScene(OBSSource source)
{
	const char *name  = obs_source_get_name(source);
	obs_scene_t *scene = obs_scene_from_source(source);

	QListWidgetItem *item = new QListWidgetItem(QT_UTF8(name));
	SetOBSRef(item, OBSScene(scene));
	ui->scenes->addItem(item);

	obs_hotkey_register_source(source, "OBSBasic.SelectScene",
			Str("Basic.Hotkeys.SelectScene"),
			[](void *data,
				obs_hotkey_id, obs_hotkey_t*, bool pressed)
	{
		OBSBasic *main =
			reinterpret_cast<OBSBasic*>(App()->GetMainWindow());

		auto potential_source = static_cast<obs_source_t*>(data);
		auto source = obs_source_get_ref(potential_source);
		if (source && pressed)
			main->SetCurrentScene(source);
		obs_source_release(source);
	}, static_cast<obs_source_t*>(source));

	signal_handler_t *handler = obs_source_get_signal_handler(source);

	SignalContainer<OBSScene> container;
	container.ref = scene;
	container.handlers.assign({
		std::make_shared<OBSSignal>(handler, "item_add",
					OBSBasic::SceneItemAdded, this),
		std::make_shared<OBSSignal>(handler, "item_remove",
					OBSBasic::SceneItemRemoved, this),
		std::make_shared<OBSSignal>(handler, "item_select",
					OBSBasic::SceneItemSelected, this),
		std::make_shared<OBSSignal>(handler, "item_deselect",
					OBSBasic::SceneItemDeselected, this),
		std::make_shared<OBSSignal>(handler, "reorder",
					OBSBasic::SceneReordered, this),
	});

	item->setData(static_cast<int>(QtDataRole::OBSSignals),
			QVariant::fromValue(container));

	/* if the scene already has items (a duplicated scene) add them */
	auto addSceneItem = [this] (obs_sceneitem_t *item)
	{
		AddSceneItem(item);
	};

	using addSceneItem_t = decltype(addSceneItem);

	obs_scene_enum_items(scene,
			[] (obs_scene_t*, obs_sceneitem_t *item, void *param)
			{
				addSceneItem_t *func;
				func = reinterpret_cast<addSceneItem_t*>(param);
				(*func)(item);
				return true;
			}, &addSceneItem);

	SaveProject();

	if (!disableSaving) {
		obs_source_t *source = obs_scene_get_source(scene);
		blog(LOG_INFO, "User added scene '%s'",
				obs_source_get_name(source));

		OBSProjector::UpdateMultiviewProjectors();
	}

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED);
}

void OBSBasic::RemoveScene(OBSSource source)
{
	obs_scene_t *scene = obs_scene_from_source(source);

	QListWidgetItem *sel = nullptr;
	int count = ui->scenes->count();

	for (int i = 0; i < count; i++) {
		auto item = ui->scenes->item(i);
		auto cur_scene = GetOBSRef<OBSScene>(item);
		if (cur_scene != scene)
			continue;

		sel = item;
		break;
	}

	if (sel != nullptr) {
		if (sel == ui->scenes->currentItem())
			ClearListItems(ui->sources);
		delete sel;
	}

	SaveProject();

	if (!disableSaving) {
		blog(LOG_INFO, "User Removed scene '%s'",
				obs_source_get_name(source));

		OBSProjector::UpdateMultiviewProjectors();
	}

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED);
}

void OBSBasic::AddSceneItem(OBSSceneItem item)
{
	obs_scene_t  *scene  = obs_sceneitem_get_scene(item);

	if (GetCurrentScene() == scene)
		InsertSceneItem(item);

	SaveProject();

	if (!disableSaving) {
		obs_source_t *sceneSource = obs_scene_get_source(scene);
		obs_source_t *itemSource = obs_sceneitem_get_source(item);
		blog(LOG_INFO, "User added source '%s' (%s) to scene '%s'",
				obs_source_get_name(itemSource),
				obs_source_get_id(itemSource),
				obs_source_get_name(sceneSource));
	}
}

void OBSBasic::RemoveSceneItem(OBSSceneItem item)
{
	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem *listItem = ui->sources->item(i);

		if (GetOBSRef<OBSSceneItem>(listItem) == item) {
			DeleteListItem(ui->sources, listItem);
			break;
		}
	}

	SaveProject();

	if (!disableSaving) {
		obs_scene_t *scene = obs_sceneitem_get_scene(item);
		obs_source_t *sceneSource = obs_scene_get_source(scene);
		obs_source_t *itemSource = obs_sceneitem_get_source(item);
		blog(LOG_INFO, "User Removed source '%s' (%s) from scene '%s'",
				obs_source_get_name(itemSource),
				obs_source_get_id(itemSource),
				obs_source_get_name(sceneSource));
	}

	// 注意：这里的scene_item并没有被销毁，还有后续操作...
	// 如果删除0点对象，先强制隐藏PPT翻页按钮，再重置...
	if (m_lpZeroSceneItem == item) {
		ui->preview->DispBtnPrev(false);
		ui->preview->DispBtnNext(false);
		ui->preview->DispBtnFoot(false,0,0,NULL);
		m_lpZeroSceneItem = NULL;
	}
	// 删除预览窗口对应的学生端麦克风按钮...
	ui->preview->doDeleteStudentBtnMic(item);
	// 注意：如果互动学生端，并没有调用 doSendCameraLiveStopCmd()，让互动学生端自己超时退出...
	// 注意：没有立即调用 doSendCameraLiveStopCmd() 的原因，避免关联数据源发生混乱...
	/*obs_source_t * lpSource = obs_sceneitem_get_source(item);
	if (astrcmpi(obs_source_get_id(lpSource), App()->InteractRtpSource()) == 0) {
		obs_data_t * lpSettings = obs_source_get_settings(lpSource);
		int nDBCameraID = obs_data_get_int(lpSettings, "camera_id");
		obs_data_release(lpSettings);
		App()->doSendCameraLiveStopCmd(nDBCameraID);
	}*/
}

void OBSBasic::UpdateSceneSelection(OBSSource source)
{
	if (source) {
		obs_scene_t *scene = obs_scene_from_source(source);
		const char *name = obs_source_get_name(source);

		if (!scene)
			return;

		QList<QListWidgetItem*> items =
			ui->scenes->findItems(QT_UTF8(name), Qt::MatchExactly);

		if (items.count()) {
			sceneChanging = true;
			ui->scenes->setCurrentItem(items.first());
			sceneChanging = false;

			UpdateSources(scene);
		}
	}
}

static void RenameListValues(QListWidget *listWidget, const QString &newName,
		const QString &prevName)
{
	QList<QListWidgetItem*> items =
		listWidget->findItems(prevName, Qt::MatchExactly);

	for (int i = 0; i < items.count(); i++)
		items[i]->setText(newName);
}

void OBSBasic::RenameSources(OBSSource source, QString newName,
		QString prevName)
{
	RenameListValues(ui->scenes,  newName, prevName);

	for (size_t i = 0; i < volumes.size(); i++) {
		if (volumes[i]->GetName().compare(prevName) == 0)
			volumes[i]->SetName(newName);
	}

	OBSProjector::RenameProjector(prevName, newName);

	SaveProject();

	obs_scene_t *scene = obs_scene_from_source(source);
	if (scene)
		OBSProjector::UpdateMultiviewProjectors();
}

void OBSBasic::SelectSceneItem(OBSScene scene, OBSSceneItem item, bool select)
{
	SignalBlocker sourcesSignalBlocker(ui->sources);

	if (scene != GetCurrentScene() || ignoreSelectionUpdate)
		return;

	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem *witem = ui->sources->item(i);
		QVariant data =
			witem->data(static_cast<int>(QtDataRole::OBSRef));
		if (!data.canConvert<OBSSceneItem>())
			continue;

		if (item != data.value<OBSSceneItem>())
			continue;

		witem->setSelected(select);
		break;
	}
}

static inline bool SourceMixerHidden(obs_source_t *source)
{
	obs_data_t *priv_settings = obs_source_get_private_settings(source);
	bool hidden = obs_data_get_bool(priv_settings, "mixer_hidden");
	obs_data_release(priv_settings);

	return hidden;
}

static inline void SetSourceMixerHidden(obs_source_t *source, bool hidden)
{
	obs_data_t *priv_settings = obs_source_get_private_settings(source);
	obs_data_set_bool(priv_settings, "mixer_hidden", hidden);
	obs_data_release(priv_settings);
}

void OBSBasic::GetAudioSourceFilters()
{
	QAction *action = reinterpret_cast<QAction*>(sender());
	VolControl *vol = action->property("volControl").value<VolControl*>();
	obs_source_t *source = vol->GetSource();

	CreateFiltersWindow(source);
}

void OBSBasic::GetAudioSourceProperties()
{
	QAction *action = reinterpret_cast<QAction*>(sender());
	VolControl *vol = action->property("volControl").value<VolControl*>();
	obs_source_t *source = vol->GetSource();

	CreatePropertiesWindow(source);
}

void OBSBasic::HideAudioControl()
{
	QAction *action = reinterpret_cast<QAction*>(sender());
	VolControl *vol = action->property("volControl").value<VolControl*>();
	obs_source_t *source = vol->GetSource();

	if (!SourceMixerHidden(source)) {
		SetSourceMixerHidden(source, true);
		DeactivateAudioSource(source);
	}
}

void OBSBasic::UnhideAllAudioControls()
{
	auto UnhideAudioMixer = [this] (obs_source_t *source) /* -- */
	{
		if (!obs_source_active(source))
			return true;
		if (!SourceMixerHidden(source))
			return true;

		SetSourceMixerHidden(source, false);
		ActivateAudioSource(source);
		return true;
	};

	using UnhideAudioMixer_t = decltype(UnhideAudioMixer);

	auto PreEnum = [] (void *data, obs_source_t *source) -> bool /* -- */
	{
		return (*reinterpret_cast<UnhideAudioMixer_t *>(data))(source);
	};

	obs_enum_sources(PreEnum, &UnhideAudioMixer);
}

void OBSBasic::ToggleHideMixer()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_source(item);

	if (!SourceMixerHidden(source)) {
		SetSourceMixerHidden(source, true);
		DeactivateAudioSource(source);
	} else {
		SetSourceMixerHidden(source, false);
		ActivateAudioSource(source);
	}
}

void OBSBasic::MixerRenameSource()
{
	QAction *action = reinterpret_cast<QAction*>(sender());
	VolControl *vol = action->property("volControl").value<VolControl*>();
	OBSSource source = vol->GetSource();

	const char *prevName = obs_source_get_name(source);

	for (;;) {
		string name;
		bool accepted = NameDialog::AskForName(this,
				QTStr("Basic.Main.MixerRename.Title"),
				QTStr("Basic.Main.MixerRename.Text"),
				name,
				QT_UTF8(prevName));
		if (!accepted)
			return;

		if (name.empty()) {
			OBSMessageBox::information(this,
					QTStr("NoNameEntered.Title"),
					QTStr("NoNameEntered.Text"));
			continue;
		}

		OBSSource sourceTest = obs_get_source_by_name(name.c_str());
		obs_source_release(sourceTest);

		if (sourceTest) {
			OBSMessageBox::information(this,
					QTStr("NameExists.Title"),
					QTStr("NameExists.Text"));
			continue;
		}

		obs_source_set_name(source, name.c_str());
		break;
	}
}

void OBSBasic::VolControlContextMenu()
{
	VolControl *vol = reinterpret_cast<VolControl*>(sender());

	/* ------------------- */

	QAction hideAction(QTStr("Hide"), this);
	QAction unhideAllAction(QTStr("UnhideAll"), this);
	QAction mixerRenameAction(QTStr("Rename"), this);

	QAction filtersAction(QTStr("Filters"), this);
	QAction propertiesAction(QTStr("Properties"), this);
	QAction advPropAction(QTStr("Basic.MainMenu.Edit.AdvAudio"), this);

	/* ------------------- */

	connect(&hideAction, &QAction::triggered,
			this, &OBSBasic::HideAudioControl,
			Qt::DirectConnection);
	connect(&unhideAllAction, &QAction::triggered,
			this, &OBSBasic::UnhideAllAudioControls,
			Qt::DirectConnection);
	connect(&mixerRenameAction, &QAction::triggered,
			this, &OBSBasic::MixerRenameSource,
			Qt::DirectConnection);

	connect(&filtersAction, &QAction::triggered,
			this, &OBSBasic::GetAudioSourceFilters,
			Qt::DirectConnection);
	connect(&propertiesAction, &QAction::triggered,
			this, &OBSBasic::GetAudioSourceProperties,
			Qt::DirectConnection);
	connect(&advPropAction, &QAction::triggered,
			this, &OBSBasic::on_actionAdvAudioProperties_triggered,
			Qt::DirectConnection);

	/* ------------------- */

	hideAction.setProperty("volControl",
			QVariant::fromValue<VolControl*>(vol));
	mixerRenameAction.setProperty("volControl",
			QVariant::fromValue<VolControl*>(vol));

	filtersAction.setProperty("volControl",
			QVariant::fromValue<VolControl*>(vol));
	propertiesAction.setProperty("volControl",
			QVariant::fromValue<VolControl*>(vol));

	/* ------------------- */

	QMenu popup(this);
	popup.addAction(&unhideAllAction);
	popup.addAction(&hideAction);
	popup.addAction(&mixerRenameAction);
	popup.addSeparator();
	popup.addAction(&filtersAction);
	popup.addAction(&propertiesAction);
	popup.addAction(&advPropAction);
	popup.exec(QCursor::pos());
}

void OBSBasic::on_mixerScrollArea_customContextMenuRequested()
{
	QAction unhideAllAction(QTStr("UnhideAll"), this);

	QAction advPropAction(QTStr("Basic.MainMenu.Edit.AdvAudio"), this);

	/* ------------------- */

	connect(&unhideAllAction, &QAction::triggered,
			this, &OBSBasic::UnhideAllAudioControls,
			Qt::DirectConnection);

	connect(&advPropAction, &QAction::triggered,
			this, &OBSBasic::on_actionAdvAudioProperties_triggered,
			Qt::DirectConnection);

	/* ------------------- */

	QMenu popup(this);
	popup.addAction(&unhideAllAction);
	popup.addSeparator();
	popup.addAction(&advPropAction);
	popup.exec(QCursor::pos());
}

void OBSBasic::ActivateAudioSource(OBSSource source)
{
	if (SourceMixerHidden(source))
		return;

	VolControl *vol = new VolControl(source, true);

	double meterDecayRate = config_get_double(basicConfig, "Audio",
			"MeterDecayRate");
	vol->SetMeterDecayRate(meterDecayRate);
	vol->setContextMenuPolicy(Qt::CustomContextMenu);

	connect(vol, &QWidget::customContextMenuRequested,
			this, &OBSBasic::VolControlContextMenu);
	connect(vol, &VolControl::ConfigClicked,
			this, &OBSBasic::VolControlContextMenu);

	InsertQObjectByName(volumes, vol);

	for (auto volume : volumes) {
		ui->volumeWidgets->layout()->addWidget(volume);
	}
}

void OBSBasic::DeactivateAudioSource(OBSSource source)
{
	for (size_t i = 0; i < volumes.size(); i++) {
		if (volumes[i]->GetSource() == source) {
			delete volumes[i];
			volumes.erase(volumes.begin() + i);
			break;
		}
	}
}

bool OBSBasic::QueryRemoveSource(obs_source_t *source)
{
	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
		int count = ui->scenes->count();

		if (count == 1) {
			OBSMessageBox::information(this,
						QTStr("FinalScene.Title"),
						QTStr("FinalScene.Text"));
			return false;
		}
	}

	const char *name  = obs_source_get_name(source);

	QString text = QTStr("ConfirmRemove.Text");
	text.replace("$1", QT_UTF8(name));

	QMessageBox remove_source(this);
	remove_source.setText(text);
	QAbstractButton *Yes = remove_source.addButton(QTStr("Yes"),
			QMessageBox::YesRole);
	remove_source.addButton(QTStr("No"), QMessageBox::NoRole);
	remove_source.setIcon(QMessageBox::Question);
	remove_source.setWindowTitle(QTStr("ConfirmRemove.Title"));
	remove_source.exec();

	return Yes == remove_source.clickedButton();
}

void OBSBasic::CheckForUpdates(bool manualUpdate)
{
#ifdef UPDATE_SPARKLE
	trigger_sparkle_update();
#elif ENABLE_WIN_UPDATER
	// 屏蔽升级操作菜单，避免在升级过程中重复升级...
	ui->actionCheckForUpdates->setEnabled(false);
	if (updateCheckThread && updateCheckThread->isRunning())
		return;
	// 创建升级线程，并启动之 => 可以手动直接升级...
	updateCheckThread = new AutoUpdateThread(manualUpdate);
	updateCheckThread->start();
#endif

	UNUSED_PARAMETER(manualUpdate);
}

void OBSBasic::updateCheckFinished()
{
	ui->actionCheckForUpdates->setEnabled(true);
}

void OBSBasic::DuplicateSelectedScene()
{
	OBSScene curScene = GetCurrentScene();

	if (!curScene)
		return;

	OBSSource curSceneSource = obs_scene_get_source(curScene);
	QString format{obs_source_get_name(curSceneSource)};
	format += " %1";

	int i = 2;
	QString placeHolderText = format.arg(i);
	obs_source_t *source = nullptr;
	while ((source = obs_get_source_by_name(QT_TO_UTF8(placeHolderText)))) {
		obs_source_release(source);
		placeHolderText = format.arg(++i);
	}

	for (;;) {
		string name;
		bool accepted = NameDialog::AskForName(this,
				QTStr("Basic.Main.AddSceneDlg.Title"),
				QTStr("Basic.Main.AddSceneDlg.Text"),
				name,
				placeHolderText);
		if (!accepted)
			return;

		if (name.empty()) {
			OBSMessageBox::information(this,
					QTStr("NoNameEntered.Title"),
					QTStr("NoNameEntered.Text"));
			continue;
		}

		obs_source_t *source = obs_get_source_by_name(name.c_str());
		if (source) {
			OBSMessageBox::information(this,
					QTStr("NameExists.Title"),
					QTStr("NameExists.Text"));

			obs_source_release(source);
			continue;
		}

		obs_scene_t *scene = obs_scene_duplicate(curScene,
				name.c_str(), OBS_SCENE_DUP_REFS);
		source = obs_scene_get_source(scene);
		AddScene(source);
		SetCurrentScene(source, true);
		obs_scene_release(scene);

		break;
	}
}

void OBSBasic::RemoveSelectedScene()
{
	OBSScene scene = GetCurrentScene();
	if (scene) {
		obs_source_t *source = obs_scene_get_source(scene);
		if (QueryRemoveSource(source)) {
			obs_source_remove(source);

			if (api)
				api->on_event(OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED);
		}
	}
}

void OBSBasic::RemoveSelectedSceneItem()
{
	OBSSceneItem item = GetCurrentSceneItem();
	if (item) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (QueryRemoveSource(source))
			obs_sceneitem_remove(item);
	}
}

struct ReorderInfo {
	int idx = 0;
	OBSBasic *window;

	inline ReorderInfo(OBSBasic *window_) : window(window_) {}
};

void OBSBasic::ReorderSceneItem(obs_sceneitem_t *item, size_t idx)
{
	int count = ui->sources->count();
	int idx_inv = count - (int)idx - 1;

	for (int i = 0; i < count; i++) {
		QListWidgetItem *listItem = ui->sources->item(i);
		OBSSceneItem sceneItem = GetOBSRef<OBSSceneItem>(listItem);

		if (sceneItem == item) {
			if ((int)idx_inv != i) {
				bool sel = (ui->sources->currentRow() == i);

				listItem = TakeListItem(ui->sources, i);
				if (listItem)  {
					ui->sources->insertItem(idx_inv,
							listItem);
					SetupVisibilityItem(ui->sources,
							listItem, item);

					if (sel)
						ui->sources->setCurrentRow(
								idx_inv);
				}
			}

			break;
		}
	}
}

void OBSBasic::ReorderSources(OBSScene scene)
{
	ReorderInfo info(this);

	if (scene != GetCurrentScene() || ui->sources->IgnoreReorder())
		return;

	obs_scene_enum_items(scene,
			[] (obs_scene_t*, obs_sceneitem_t *item, void *p)
			{
				ReorderInfo *info =
					reinterpret_cast<ReorderInfo*>(p);

				info->window->ReorderSceneItem(item,
					info->idx++);
				return true;
			}, &info);

	SaveProject();
}

/* OBS Callbacks */

void OBSBasic::SourceUpdated(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);
	obs_source_t *source = (obs_source_t*)calldata_ptr(params, "source");
	QMetaObject::invokeMethod(window, "UpdatedSourceItem",
			Q_ARG(OBSSource, OBSSource(source)));
}

void OBSBasic::SourceMonitoring(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);
	obs_source_t *source = (obs_source_t*)calldata_ptr(params, "source");
	QMetaObject::invokeMethod(window, "MonitoringSourceChanged",
			Q_ARG(OBSSource, OBSSource(source)));
}

void OBSBasic::SceneReordered(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);

	obs_scene_t *scene = (obs_scene_t*)calldata_ptr(params, "scene");

	QMetaObject::invokeMethod(window, "ReorderSources",
			Q_ARG(OBSScene, OBSScene(scene)));
}

void OBSBasic::SceneItemAdded(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);

	obs_sceneitem_t *item = (obs_sceneitem_t*)calldata_ptr(params, "item");

	QMetaObject::invokeMethod(window, "AddSceneItem",
			Q_ARG(OBSSceneItem, OBSSceneItem(item)));
}

void OBSBasic::SceneItemRemoved(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);

	obs_sceneitem_t *item = (obs_sceneitem_t*)calldata_ptr(params, "item");

	QMetaObject::invokeMethod(window, "RemoveSceneItem",
			Q_ARG(OBSSceneItem, OBSSceneItem(item)));
}

void OBSBasic::SceneItemSelected(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);

	obs_scene_t     *scene = (obs_scene_t*)calldata_ptr(params, "scene");
	obs_sceneitem_t *item  = (obs_sceneitem_t*)calldata_ptr(params, "item");

	QMetaObject::invokeMethod(window, "SelectSceneItem",
			Q_ARG(OBSScene, scene), Q_ARG(OBSSceneItem, item),
			Q_ARG(bool, true));
}

void OBSBasic::SceneItemDeselected(void *data, calldata_t *params)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);

	obs_scene_t     *scene = (obs_scene_t*)calldata_ptr(params, "scene");
	obs_sceneitem_t *item  = (obs_sceneitem_t*)calldata_ptr(params, "item");

	QMetaObject::invokeMethod(window, "SelectSceneItem",
			Q_ARG(OBSScene, scene), Q_ARG(OBSSceneItem, item),
			Q_ARG(bool, false));

}

void OBSBasic::SourceLoaded(void *data, obs_source_t *source)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);

	if (obs_scene_from_source(source) != NULL)
		QMetaObject::invokeMethod(window,
				"AddScene",
				Q_ARG(OBSSource, OBSSource(source)));
}

void OBSBasic::SourceRemoved(void *data, calldata_t *params)
{
	obs_source_t *source = (obs_source_t*)calldata_ptr(params, "source");

	if (obs_scene_from_source(source) != NULL)
		QMetaObject::invokeMethod(static_cast<OBSBasic*>(data),
				"RemoveScene",
				Q_ARG(OBSSource, OBSSource(source)));
}

void OBSBasic::SourceActivated(void *data, calldata_t *params)
{
	obs_source_t *source = (obs_source_t*)calldata_ptr(params, "source");
	uint32_t     flags  = obs_source_get_output_flags(source);

	if (flags & OBS_SOURCE_AUDIO)
		QMetaObject::invokeMethod(static_cast<OBSBasic*>(data),
				"ActivateAudioSource",
				Q_ARG(OBSSource, OBSSource(source)));
}

void OBSBasic::SourceDeactivated(void *data, calldata_t *params)
{
	obs_source_t *source = (obs_source_t*)calldata_ptr(params, "source");
	uint32_t     flags  = obs_source_get_output_flags(source);

	if (flags & OBS_SOURCE_AUDIO)
		QMetaObject::invokeMethod(static_cast<OBSBasic*>(data),
				"DeactivateAudioSource",
				Q_ARG(OBSSource, OBSSource(source)));
}

void OBSBasic::SourceRenamed(void *data, calldata_t *params)
{
	obs_source_t *source = (obs_source_t*)calldata_ptr(params, "source");
	const char *newName  = calldata_string(params, "new_name");
	const char *prevName = calldata_string(params, "prev_name");

	QMetaObject::invokeMethod(static_cast<OBSBasic*>(data),
			"RenameSources",
			Q_ARG(OBSSource, source),
			Q_ARG(QString, QT_UTF8(newName)),
			Q_ARG(QString, QT_UTF8(prevName)));

	blog(LOG_INFO, "Source '%s' renamed to '%s'", prevName, newName);
}

void OBSBasic::DrawBackdrop(float cx, float cy)
{
	if (!box)
		return;

	gs_effect_t    *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t    *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech  = gs_effect_get_technique(solid, "Solid");

	vec4 colorVal;
	vec4_set(&colorVal, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color, &colorVal);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_scale3f(float(cx), float(cy), 1.0f);

	gs_load_vertexbuffer(box);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	gs_load_vertexbuffer(nullptr);
}

void OBSBasic::RenderMain(void *data, uint32_t cx, uint32_t cy)
{
	OBSBasic *window = static_cast<OBSBasic*>(data);
	obs_video_info ovi;

	obs_get_video_info(&ovi);

	window->previewCX = int(window->previewScale * float(ovi.base_width));
	window->previewCY = int(window->previewScale * float(ovi.base_height));

	gs_viewport_push();
	gs_projection_push();

	/* --------------------------------------- */

	gs_ortho(0.0f, float(ovi.base_width), 0.0f, float(ovi.base_height),
			-100.0f, 100.0f);
	gs_set_viewport(window->previewX, window->previewY,
			window->previewCX, window->previewCY);

	window->DrawBackdrop(float(ovi.base_width), float(ovi.base_height));

	if (window->IsPreviewProgramMode()) {
		OBSScene scene = window->GetCurrentScene();
		obs_source_t *source = obs_scene_get_source(scene);
		if (source)
			obs_source_video_render(source);
	} else {
		obs_render_main_texture();
	}
	gs_load_vertexbuffer(nullptr);

	/* --------------------------------------- */

	QSize previewSize = GetPixelSize(window->ui->preview);
	float right  = float(previewSize.width())  - window->previewX;
	float bottom = float(previewSize.height()) - window->previewY;

	gs_ortho(-window->previewX, right,
	         -window->previewY, bottom,
	         -100.0f, 100.0f);
	gs_reset_viewport();

	window->ui->preview->DrawSceneEditing();

	/* --------------------------------------- */

	gs_projection_pop();
	gs_viewport_pop();

	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
}

/* Main class functions */

obs_service_t *OBSBasic::GetService()
{
	if (!service) {
		service = obs_service_create("rtmp_common", NULL, NULL,
				nullptr);
		obs_service_release(service);
	}
	return service;
}

void OBSBasic::SetService(obs_service_t *newService)
{
	if (newService)
		service = newService;
}

bool OBSBasic::StreamingActive() const
{
	if (!outputHandler)
		return false;
	return outputHandler->StreamingActive();
}

bool OBSBasic::Active() const
{
	if (!outputHandler)
		return false;
	return outputHandler->Active();
}

#ifdef _WIN32
#define IS_WIN32 1
#else
#define IS_WIN32 0
#endif

static inline int AttemptToResetVideo(struct obs_video_info *ovi)
{
	return obs_reset_video(ovi);
}

static inline enum obs_scale_type GetScaleType(ConfigFile &basicConfig)
{
	const char *scaleTypeStr = config_get_string(basicConfig,
			"Video", "ScaleType");

	if (astrcmpi(scaleTypeStr, "bilinear") == 0)
		return OBS_SCALE_BILINEAR;
	else if (astrcmpi(scaleTypeStr, "lanczos") == 0)
		return OBS_SCALE_LANCZOS;
	else
		return OBS_SCALE_BICUBIC;
}

static inline enum video_format GetVideoFormatFromName(const char *name)
{
	if (astrcmpi(name, "I420") == 0)
		return VIDEO_FORMAT_I420;
	else if (astrcmpi(name, "NV12") == 0)
		return VIDEO_FORMAT_NV12;
	else if (astrcmpi(name, "I444") == 0)
		return VIDEO_FORMAT_I444;
#if 0 //currently unsupported
	else if (astrcmpi(name, "YVYU") == 0)
		return VIDEO_FORMAT_YVYU;
	else if (astrcmpi(name, "YUY2") == 0)
		return VIDEO_FORMAT_YUY2;
	else if (astrcmpi(name, "UYVY") == 0)
		return VIDEO_FORMAT_UYVY;
#endif
	else
		return VIDEO_FORMAT_RGBA;
}

void OBSBasic::ResetUI()
{
	bool studioPortraitLayout = config_get_bool(GetGlobalConfig(),
			"BasicWindow", "StudioPortraitLayout");

	if (studioPortraitLayout)
		ui->previewLayout->setDirection(QBoxLayout::TopToBottom);
	else
		ui->previewLayout->setDirection(QBoxLayout::LeftToRight);
}

int OBSBasic::ResetVideo()
{
	if (outputHandler && outputHandler->Active())
		return OBS_VIDEO_CURRENTLY_ACTIVE;

	ProfileScope("OBSBasic::ResetVideo");

	struct obs_video_info ovi = { 0 };
	int ret = -1;

	GetConfigFPS(ovi.fps_num, ovi.fps_den);

	const char *colorFormat = config_get_string(basicConfig, "Video",
			"ColorFormat");
	const char *colorSpace = config_get_string(basicConfig, "Video",
			"ColorSpace");
	const char *colorRange = config_get_string(basicConfig, "Video",
			"ColorRange");

	ovi.graphics_module = App()->GetRenderModule();
	ovi.base_width     = (uint32_t)config_get_uint(basicConfig,
			"Video", "BaseCX");
	ovi.base_height    = (uint32_t)config_get_uint(basicConfig,
			"Video", "BaseCY");
	ovi.output_width   = (uint32_t)config_get_uint(basicConfig,
			"Video", "OutputCX");
	ovi.output_height  = (uint32_t)config_get_uint(basicConfig,
			"Video", "OutputCY");
	ovi.output_format  = GetVideoFormatFromName(colorFormat);
	ovi.colorspace     = astrcmpi(colorSpace, "601") == 0 ?
		VIDEO_CS_601 : VIDEO_CS_709;
	ovi.range          = astrcmpi(colorRange, "Full") == 0 ?
		VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	ovi.adapter        = config_get_uint(App()->GlobalConfig(),
			"Video", "AdapterIdx");
	ovi.gpu_conversion = true;
	ovi.screen_mode    = false;
	ovi.scale_type     = GetScaleType(basicConfig);

	if (ovi.base_width == 0 || ovi.base_height == 0) {
		ovi.base_width = 1920;
		ovi.base_height = 1080;
		config_set_uint(basicConfig, "Video", "BaseCX", 1920);
		config_set_uint(basicConfig, "Video", "BaseCY", 1080);
	}

	if (ovi.output_width == 0 || ovi.output_height == 0) {
		ovi.output_width = ovi.base_width;
		ovi.output_height = ovi.base_height;
		config_set_uint(basicConfig, "Video", "OutputCX",
				ovi.base_width);
		config_set_uint(basicConfig, "Video", "OutputCY",
				ovi.base_height);
	}

	ret = AttemptToResetVideo(&ovi);
	if (IS_WIN32 && ret != OBS_VIDEO_SUCCESS) {
		if (ret == OBS_VIDEO_CURRENTLY_ACTIVE) {
			blog(LOG_WARNING, "Tried to reset when "
			                  "already active");
			return ret;
		}
		// 弹框询问是否安装D3D或者使用OpenGL => 父窗口设定为空，否则无法居中显示...
		QMessageBox::StandardButton button = OBSMessageBox::question(
			NULL, QTStr("ConfirmD3D.Title"), QTStr("ConfirmD3D.Text"),
			QMessageBox::Yes | QMessageBox::No);
		if (button == QMessageBox::Yes) {
			return this->doD3DSetup();
		}
		/* Try OpenGL if DirectX fails on windows */
		if (astrcmpi(ovi.graphics_module, DL_OPENGL) != 0) {
			blog(LOG_WARNING, "Failed to initialize obs video (%d) "
					  "with graphics_module='%s', retrying "
					  "with graphics_module='%s'",
					  ret, ovi.graphics_module,
					  DL_OPENGL);
			ovi.graphics_module = DL_OPENGL;
			ret = AttemptToResetVideo(&ovi);
		}
	} else if (ret == OBS_VIDEO_SUCCESS) {
		ResizePreview(ovi.base_width, ovi.base_height);
		if (program)
			ResizeProgram(ovi.base_width, ovi.base_height);
	}

	if (ret == OBS_VIDEO_SUCCESS) {
		OBSBasicStats::InitializeValues();
	}
	return ret;
}

bool OBSBasic::ResetAudio()
{
	ProfileScope("OBSBasic::ResetAudio");

	struct obs_audio_info ai;
	ai.samples_per_sec = config_get_uint(basicConfig, "Audio",
			"SampleRate");

	const char *channelSetupStr = config_get_string(basicConfig,
			"Audio", "ChannelSetup");

	if (strcmp(channelSetupStr, "Mono") == 0)
		ai.speakers = SPEAKERS_MONO;
	else if (strcmp(channelSetupStr, "2.1") == 0)
		ai.speakers = SPEAKERS_2POINT1;
	else if (strcmp(channelSetupStr, "4.0") == 0)
		ai.speakers = SPEAKERS_4POINT0;
	else if (strcmp(channelSetupStr, "4.1") == 0)
		ai.speakers = SPEAKERS_4POINT1;
	else if (strcmp(channelSetupStr, "5.1") == 0)
		ai.speakers = SPEAKERS_5POINT1;
	else if (strcmp(channelSetupStr, "7.1") == 0)
		ai.speakers = SPEAKERS_7POINT1;
	else
		ai.speakers = SPEAKERS_STEREO;

	return obs_reset_audio(&ai);
}

void OBSBasic::ResetAudioDevice(const char *sourceId, const char *deviceId,	const char *deviceDesc, int channel)
{
	bool disable = deviceId && strcmp(deviceId, "disabled") == 0;
	obs_source_t *source;
	obs_data_t *settings;

	source = obs_get_output_source(channel);
	if (source) {
		if (disable) {
			obs_set_output_source(channel, nullptr);
		} else {
			settings = obs_source_get_settings(source);
			const char *oldId = obs_data_get_string(settings, "device_id");
			if (strcmp(oldId, deviceId) != 0) {
				obs_data_set_string(settings, "device_id", deviceId);
				obs_source_update(source, settings);
			}
			obs_data_release(settings);
		}

		obs_source_release(source);

	} else if (!disable) {
		settings = obs_data_create();
		obs_data_set_string(settings, "device_id", deviceId);
		source = obs_source_create(sourceId, deviceDesc, settings, nullptr);
		obs_data_release(settings);
		obs_set_output_source(channel, source);
		obs_source_release(source);
		// 如果是音频输入设备，自动加入噪音抑制过滤器|自动屏蔽第三轨道混音...
		if (strcmp(sourceId, App()->InputAudioSource()) == 0) {
			OBSBasicSourceSelect::AddFilterToSourceByID(source, App()->GetNSFilter());
			// 思路错误 => 轨道3(索引编号是2)专门用来本地统一播放使用的混音通道...
			//uint32_t new_mixers = obs_source_get_audio_mixers(source) & (~(1 << 2));
			//obs_source_set_audio_mixers(source, new_mixers);
		}
		// 如果是音频输出设备，自动设置为静音状态，避免发生多次叠加啸叫...
		if (strcmp(sourceId, App()->OutputAudioSource()) == 0) {
			obs_source_set_muted(source, true);
		}
	}
}

void OBSBasic::ResizePreview(uint32_t cx, uint32_t cy)
{
	QSize  targetSize;
	bool isFixedScaling;
	obs_video_info ovi;

	/* resize preview panel to fix to the top section of the window */
	targetSize = GetPixelSize(ui->preview);

	isFixedScaling = ui->preview->IsFixedScaling();
	obs_get_video_info(&ovi);

	if (isFixedScaling) {
		previewScale = ui->preview->GetScalingAmount();
		GetCenterPosFromFixedScale(int(cx), int(cy),
				targetSize.width() - PREVIEW_EDGE_SIZE * 2,
				targetSize.height() - PREVIEW_EDGE_SIZE * 2,
				previewX, previewY, previewScale);
		previewX += ui->preview->GetScrollX();
		previewY += ui->preview->GetScrollY();

	} else {
		GetScaleAndCenterPos(int(cx), int(cy),
				targetSize.width() - PREVIEW_EDGE_SIZE * 2,
				targetSize.height() - PREVIEW_EDGE_SIZE * 2,
				previewX, previewY, previewScale);
	}

	previewX += float(PREVIEW_EDGE_SIZE);
	previewY += float(PREVIEW_EDGE_SIZE);
	
	ui->preview->ResizeBtnPage(ovi.base_height);
	ui->preview->ResizeBtnPPT(ovi.base_height);
	ui->preview->ResizeBtnMicAll();
}

void OBSBasic::CloseDialogs()
{
	QList<QDialog*> childDialogs = this->findChildren<QDialog *>();
	if (!childDialogs.isEmpty()) {
		for (int i = 0; i < childDialogs.size(); ++i) {
			childDialogs.at(i)->close();
		}
	}

	for (QPointer<QWidget> &projector : windowProjectors) {
		delete projector;
		projector.clear();
	}
	for (QPointer<QWidget> &projector : projectors) {
		delete projector;
		projector.clear();
	}

	if (!stats.isNull()) stats->close(); //call close to save Stats geometry
}

void OBSBasic::EnumDialogs()
{
	visDialogs.clear();
	modalDialogs.clear();
	visMsgBoxes.clear();

	/* fill list of Visible dialogs and Modal dialogs */
	QList<QDialog*> dialogs = findChildren<QDialog*>();
	for (QDialog *dialog : dialogs) {
		if (dialog->isVisible())
			visDialogs.append(dialog);
		if (dialog->isModal())
			modalDialogs.append(dialog);
	}

	/* fill list of Visible message boxes */
	QList<QMessageBox*> msgBoxes = findChildren<QMessageBox*>();
	for (QMessageBox *msgbox : msgBoxes) {
		if (msgbox->isVisible())
			visMsgBoxes.append(msgbox);
	}
}

void OBSBasic::ClearSceneData()
{
	disableSaving++;

	// 注意：如果不删除，obs核心也会主动删除...
	// 断开并删除当前scene下面的本地播放监视器...
	this->doSceneDestoryMonitor();

	CloseDialogs();

	ClearVolumeControls();
	ClearListItems(ui->scenes);
	ClearListItems(ui->sources);
	ClearQuickTransitions();
	ui->transitions->clear();

	obs_set_output_source(0, nullptr);
	obs_set_output_source(1, nullptr);
	obs_set_output_source(2, nullptr);
	obs_set_output_source(3, nullptr);
	obs_set_output_source(4, nullptr);
	obs_set_output_source(5, nullptr);
	lastScene = nullptr;
	swapScene = nullptr;
	programScene = nullptr;

	auto cb = [](void *unused, obs_source_t *source)
	{
		obs_source_remove(source);
		UNUSED_PARAMETER(unused);
		return true;
	};

	obs_enum_sources(cb, nullptr);

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP);

	disableSaving--;

	blog(LOG_INFO, "All scene data cleared");
	blog(LOG_INFO, "------------------------------------------------");
}

void OBSBasic::closeEvent(QCloseEvent *event)
{
	QMessageBox::StandardButton button;
	// 如果不是沉默退出，需要弹出退出询问框...
	if (!this->m_bIsSlientClose) {
		button = OBSMessageBox::question(
			this, QTStr("ConfirmExit.Title"),
			QTStr("ConfirmExit.Quit"));
		if (button == QMessageBox::No) {
			event->ignore();
			return;
		}
		// 如果窗口可见，需要保持坐标...
		if (this->isVisible()) {
			config_set_string(App()->GlobalConfig(),
				"BasicWindow", "geometry",
				saveGeometry().toBase64().constData());
		}
		// 保存各个Docker的状态信息...
		config_set_string(App()->GlobalConfig(),
				"BasicWindow", "DockState",
				saveState().toBase64().constData());
	}
	// 如果不是沉默退出，并且正在推流，需要弹出停止推流询问框...
	if (!this->m_bIsSlientClose && outputHandler && outputHandler->Active()) {
		this->SetShowing(true);
		button = OBSMessageBox::question(
				this, QTStr("ConfirmExit.Title"),
				QTStr("ConfirmExit.Text"));
		if (button == QMessageBox::No) {
			event->ignore();
			return;
		}
	}

	QWidget::closeEvent(event);
	if (!event->isAccepted())
		return;

	blog(LOG_INFO, SHUTDOWN_SEPARATOR);

	if (updateCheckThread)
		updateCheckThread->wait();
	if (logUploadThread)
		logUploadThread->wait();

	signalHandlers.clear();

	SaveProjectNow();

	if (api) {
		api->on_event(OBS_FRONTEND_EVENT_EXIT);
	}
	disableSaving++;

	/* Clear all scene data (dialogs, widgets, widget sub-items, scenes,
	 * sources, etc) so that all references are released before shutdown */
	ClearSceneData();

	// 调用退出事件通知...
	App()->doLogoutEvent();
	// 调用关闭退出接口...
	App()->quit();
}

void OBSBasic::changeEvent(QEvent *event)
{
	if (event->type() == QEvent::WindowStateChange &&
	    isMinimized() &&
	    trayIcon &&
	    trayIcon->isVisible() &&
	    sysTrayMinimizeToTray()) {

		ToggleShowHide();
	}
}

void OBSBasic::on_actionShow_Recordings_triggered()
{
	const char *mode = config_get_string(basicConfig, "Output", "Mode");
	const char *path = strcmp(mode, "Advanced") ?
		config_get_string(basicConfig, "SimpleOutput", "FilePath") :
		config_get_string(basicConfig, "AdvOut", "RecFilePath");
	QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void OBSBasic::on_actionRemux_triggered()
{
	const char *mode = config_get_string(basicConfig, "Output", "Mode");
	const char *path = strcmp(mode, "Advanced") ?
		config_get_string(basicConfig, "SimpleOutput", "FilePath") :
		config_get_string(basicConfig, "AdvOut", "RecFilePath");
//	OBSRemux remux(path, this);
//	remux.exec();
}

void OBSBasic::on_action_Settings_triggered()
{
	OBSBasicSettings settings(this);
	settings.exec();
	SystemTray(false);
}

void OBSBasic::on_actionAdvAudioProperties_triggered()
{
	if (advAudioWindow != nullptr) {
		advAudioWindow->raise();
		return;
	}

	advAudioWindow = new OBSBasicAdvAudio(this);
	advAudioWindow->show();
	advAudioWindow->setAttribute(Qt::WA_DeleteOnClose, true);

	connect(advAudioWindow, SIGNAL(destroyed()),
		this, SLOT(on_advAudioProps_destroyed()));
}

void OBSBasic::on_advAudioProps_clicked()
{
	on_actionAdvAudioProperties_triggered();
}

void OBSBasic::on_advAudioProps_destroyed()
{
	advAudioWindow = nullptr;
}

void OBSBasic::on_scenes_currentItemChanged(QListWidgetItem *current,
		QListWidgetItem *prev)
{
	obs_source_t *source = NULL;

	if (sceneChanging)
		return;

	if (current) {
		obs_scene_t *scene;

		scene = GetOBSRef<OBSScene>(current);
		source = obs_scene_get_source(scene);
	}

	SetCurrentScene(source);

	UNUSED_PARAMETER(prev);
}

void OBSBasic::EditSceneName()
{
	QListWidgetItem *item = ui->scenes->currentItem();
	Qt::ItemFlags flags   = item->flags();

	item->setFlags(flags | Qt::ItemIsEditable);
	ui->scenes->editItem(item);
	item->setFlags(flags);
}

static void AddProjectorMenuMonitors(QMenu *parent, QObject *target,
		const char *slot)
{
	QAction *action;
	QList<QScreen*> screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		QRect screenGeometry = screens[i]->geometry();
		QString str = QString("%1 %2: %3x%4 @ %5,%6").
			arg(QTStr("Display"),
			    QString::number(i),
			    QString::number(screenGeometry.width()),
			    QString::number(screenGeometry.height()),
			    QString::number(screenGeometry.x()),
			    QString::number(screenGeometry.y()));

		action = parent->addAction(str, target, slot);
		action->setProperty("monitor", i);
	}
}

void OBSBasic::on_scenes_customContextMenuRequested(const QPoint &pos)
{
	QListWidgetItem *item = ui->scenes->itemAt(pos);
	QPointer<QMenu> sceneProjectorMenu;

	QMenu popup(this);
	QMenu order(QTStr("Basic.MainMenu.Edit.Order"), this);
	popup.addAction(QTStr("Add"),
			this, SLOT(on_actionAddScene_triggered()));

	if (item) {
		popup.addSeparator();
		popup.addAction(QTStr("Duplicate"),
				this, SLOT(DuplicateSelectedScene()));
		popup.addAction(QTStr("Rename"),
				this, SLOT(EditSceneName()));
		popup.addAction(QTStr("Remove"),
				this, SLOT(RemoveSelectedScene()));
		popup.addSeparator();

		order.addAction(QTStr("Basic.MainMenu.Edit.Order.MoveUp"),
				this, SLOT(on_actionSceneUp_triggered()));
		order.addAction(QTStr("Basic.MainMenu.Edit.Order.MoveDown"),
				this, SLOT(on_actionSceneDown_triggered()));
		order.addSeparator();
		order.addAction(QTStr("Basic.MainMenu.Edit.Order.MoveToTop"),
				this, SLOT(MoveSceneToTop()));
		order.addAction(QTStr("Basic.MainMenu.Edit.Order.MoveToBottom"),
				this, SLOT(MoveSceneToBottom()));
		popup.addMenu(&order);

		popup.addSeparator();

		sceneProjectorMenu = new QMenu(QTStr("SceneProjector"));
		AddProjectorMenuMonitors(sceneProjectorMenu, this,
				SLOT(OpenSceneProjector()));
		popup.addMenu(sceneProjectorMenu);

		QAction *sceneWindow = popup.addAction(
				QTStr("SceneWindow"),
				this, SLOT(OpenSceneWindow()));

		popup.addAction(sceneWindow);
		popup.addSeparator();
		popup.addAction(QTStr("Filters"), this,
				SLOT(OpenSceneFilters()));

		popup.addSeparator();

		QMenu *transitionMenu = CreatePerSceneTransitionMenu();
		popup.addMenu(transitionMenu);

		/* ---------------------- */

		QAction *multiviewAction = popup.addAction(
				QTStr("ShowInMultiview"));

		OBSSource source = GetCurrentSceneSource();
		OBSData data = obs_source_get_private_settings(source);
		obs_data_release(data);

		obs_data_set_default_bool(data, "show_in_multiview",
				true);
		bool show = obs_data_get_bool(data, "show_in_multiview");

		multiviewAction->setCheckable(true);
		multiviewAction->setChecked(show);

		auto showInMultiview = [this] (OBSData data)
		{
			bool show = obs_data_get_bool(data,
					"show_in_multiview");
			obs_data_set_bool(data, "show_in_multiview",
					!show);
			OBSProjector::UpdateMultiviewProjectors();
		};

		connect(multiviewAction, &QAction::triggered,
				std::bind(showInMultiview, data));
	}

	popup.exec(QCursor::pos());
}

void OBSBasic::on_actionAddScene_triggered()
{
	string name;
	QString format{QTStr("Basic.Main.DefaultSceneName.Text")};

	int i = 2;
	QString placeHolderText = format.arg(i);
	obs_source_t *source = nullptr;
	while ((source = obs_get_source_by_name(QT_TO_UTF8(placeHolderText)))) {
		obs_source_release(source);
		placeHolderText = format.arg(++i);
	}

	bool accepted = NameDialog::AskForName(this,
			QTStr("Basic.Main.AddSceneDlg.Title"),
			QTStr("Basic.Main.AddSceneDlg.Text"),
			name,
			placeHolderText);

	if (accepted) {
		if (name.empty()) {
			OBSMessageBox::information(this,
					QTStr("NoNameEntered.Title"),
					QTStr("NoNameEntered.Text"));
			on_actionAddScene_triggered();
			return;
		}

		obs_source_t *source = obs_get_source_by_name(name.c_str());
		if (source) {
			OBSMessageBox::information(this,
					QTStr("NameExists.Title"),
					QTStr("NameExists.Text"));

			obs_source_release(source);
			on_actionAddScene_triggered();
			return;
		}

		obs_scene_t *scene = obs_scene_create(name.c_str());
		source = obs_scene_get_source(scene);
		AddScene(source);
		SetCurrentScene(source);
		obs_scene_release(scene);
	}
}

void OBSBasic::on_actionRemoveScene_triggered()
{
	OBSScene     scene  = GetCurrentScene();
	obs_source_t *source = obs_scene_get_source(scene);

	if (source && QueryRemoveSource(source))
		obs_source_remove(source);
}

void OBSBasic::ChangeSceneIndex(bool relative, int offset, int invalidIdx)
{
	int idx = ui->scenes->currentRow();
	if (idx == -1 || idx == invalidIdx)
		return;

	sceneChanging = true;

	QListWidgetItem *item = ui->scenes->takeItem(idx);

	if (!relative)
		idx = 0;

	ui->scenes->insertItem(idx + offset, item);
	ui->scenes->setCurrentRow(idx + offset);
	item->setSelected(true);

	sceneChanging = false;
}

void OBSBasic::on_actionSceneUp_triggered()
{
	ChangeSceneIndex(true, -1, 0);
}

void OBSBasic::on_actionSceneDown_triggered()
{
	ChangeSceneIndex(true, 1, ui->scenes->count() - 1);
}

void OBSBasic::MoveSceneToTop()
{
	ChangeSceneIndex(false, 0, 0);
}

void OBSBasic::MoveSceneToBottom()
{
	ChangeSceneIndex(false, ui->scenes->count() - 1,
			ui->scenes->count() - 1);
}

void OBSBasic::on_sources_itemSelectionChanged()
{
	SignalBlocker sourcesSignalBlocker(ui->sources);

	auto updateItemSelection = [&]()
	{
		ignoreSelectionUpdate = true;
		for (int i = 0; i < ui->sources->count(); i++)
		{
			QListWidgetItem *wItem = ui->sources->item(i);
			OBSSceneItem item = GetOBSRef<OBSSceneItem>(wItem);

			obs_sceneitem_select(item, wItem->isSelected());
		}
		ignoreSelectionUpdate = false;
	};
	using updateItemSelection_t = decltype(updateItemSelection);

	obs_scene_atomic_update(GetCurrentScene(),
			[](void *data, obs_scene_t *)
	{
		(*static_cast<updateItemSelection_t*>(data))();
	}, static_cast<void*>(&updateItemSelection));
}

void OBSBasic::EditSceneItemName()
{
	QListWidgetItem *item = GetTopSelectedSourceItem();
	Qt::ItemFlags flags   = item->flags();
	OBSSceneItem sceneItem= GetOBSRef<OBSSceneItem>(item);
	obs_source_t *source  = obs_sceneitem_get_source(sceneItem);
	const char *name      = obs_source_get_name(source);

	item->setText(QT_UTF8(name));
	item->setFlags(flags | Qt::ItemIsEditable);
	ui->sources->removeItemWidget(item);
	ui->sources->editItem(item);
	item->setFlags(flags);
}

void OBSBasic::SetDeinterlacingMode()
{
	QAction *action = reinterpret_cast<QAction*>(sender());
	obs_deinterlace_mode mode =
		(obs_deinterlace_mode)action->property("mode").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();
	obs_source_t *source = obs_sceneitem_get_source(sceneItem);

	obs_source_set_deinterlace_mode(source, mode);
}

void OBSBasic::SetDeinterlacingOrder()
{
	QAction *action = reinterpret_cast<QAction*>(sender());
	obs_deinterlace_field_order order =
		(obs_deinterlace_field_order)action->property("order").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();
	obs_source_t *source = obs_sceneitem_get_source(sceneItem);

	obs_source_set_deinterlace_field_order(source, order);
}

QMenu *OBSBasic::AddDeinterlacingMenu(obs_source_t *source)
{
	QMenu *menu = new QMenu(QTStr("Deinterlacing"));
	obs_deinterlace_mode deinterlaceMode =
		obs_source_get_deinterlace_mode(source);
	obs_deinterlace_field_order deinterlaceOrder =
		obs_source_get_deinterlace_field_order(source);
	QAction *action;

#define ADD_MODE(name, mode) \
	action = menu->addAction(QTStr("" name), this, \
				SLOT(SetDeinterlacingMode())); \
	action->setProperty("mode", (int)mode); \
	action->setCheckable(true); \
	action->setChecked(deinterlaceMode == mode);

	ADD_MODE("Disable",                OBS_DEINTERLACE_MODE_DISABLE);
	ADD_MODE("Deinterlacing.Discard",  OBS_DEINTERLACE_MODE_DISCARD);
	ADD_MODE("Deinterlacing.Retro",    OBS_DEINTERLACE_MODE_RETRO);
	ADD_MODE("Deinterlacing.Blend",    OBS_DEINTERLACE_MODE_BLEND);
	ADD_MODE("Deinterlacing.Blend2x",  OBS_DEINTERLACE_MODE_BLEND_2X);
	ADD_MODE("Deinterlacing.Linear",   OBS_DEINTERLACE_MODE_LINEAR);
	ADD_MODE("Deinterlacing.Linear2x", OBS_DEINTERLACE_MODE_LINEAR_2X);
	ADD_MODE("Deinterlacing.Yadif",    OBS_DEINTERLACE_MODE_YADIF);
	ADD_MODE("Deinterlacing.Yadif2x",  OBS_DEINTERLACE_MODE_YADIF_2X);
#undef ADD_MODE

	menu->addSeparator();

#define ADD_ORDER(name, order) \
	action = menu->addAction(QTStr("Deinterlacing." name), this, \
				SLOT(SetDeinterlacingOrder())); \
	action->setProperty("order", (int)order); \
	action->setCheckable(true); \
	action->setChecked(deinterlaceOrder == order);

	ADD_ORDER("TopFieldFirst",    OBS_DEINTERLACE_FIELD_ORDER_TOP);
	ADD_ORDER("BottomFieldFirst", OBS_DEINTERLACE_FIELD_ORDER_BOTTOM);
#undef ADD_ORDER

	return menu;
}

void OBSBasic::SetScaleFilter()
{
	QAction *action = reinterpret_cast<QAction*>(sender());
	obs_scale_type mode = (obs_scale_type)action->property("mode").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();

	obs_sceneitem_set_scale_filter(sceneItem, mode);
}

QMenu *OBSBasic::AddScaleFilteringMenu(obs_sceneitem_t *item)
{
	QMenu *menu = new QMenu(QTStr("ScaleFiltering"));
	obs_scale_type scaleFilter = obs_sceneitem_get_scale_filter(item);
	QAction *action;

#define ADD_MODE(name, mode) \
	action = menu->addAction(QTStr("" name), this, \
				SLOT(SetScaleFilter())); \
	action->setProperty("mode", (int)mode); \
	action->setCheckable(true); \
	action->setChecked(scaleFilter == mode);

	ADD_MODE("Disable",                 OBS_SCALE_DISABLE);
	ADD_MODE("ScaleFiltering.Point",    OBS_SCALE_POINT);
	ADD_MODE("ScaleFiltering.Bilinear", OBS_SCALE_BILINEAR);
	ADD_MODE("ScaleFiltering.Bicubic",  OBS_SCALE_BICUBIC);
	ADD_MODE("ScaleFiltering.Lanczos",  OBS_SCALE_LANCZOS);
#undef ADD_MODE

	return menu;
}

void OBSBasic::OpenWindowPTZ()
{
	// 创建云台控制窗口...
	if (m_PTZWindow == NULL) {
		m_PTZWindow = new CPTZWindow(this);
	}
	// 注意：远程云台控制，只管发命令，不用处理反馈结果...
	// 显示云台控制窗口，点击鼠标时已经更新摄像头编号...
	m_PTZWindow->show();
}

void OBSBasic::OpenFloatSource()
{
	// 通过标准接口，查找当前已选中数据源...
	OBSScene scene = this->GetCurrentScene();
	OBSSceneItem curItem = this->GetCurrentSceneItem();
	if (scene == NULL || curItem == NULL)
		return;
	// 先置顶当前数据源，再浮动已选中的当前数据源对象...
	obs_sceneitem_set_order(curItem, OBS_ORDER_MOVE_TOP);
	obs_sceneitem_set_floated(curItem, true);
	// 获取显示系统的宽和高...
	obs_video_info ovi = { 0 };
	obs_get_video_info(&ovi);
	// 将当前数据源移动到右上角...
	vec2 vBounds, vPos, vFind;
	obs_sceneitem_t * lpItemFind = NULL;
	obs_sceneitem_get_bounds(curItem, &vBounds);
	float xPos = ovi.base_width;
	while (xPos > 0.0f) {
		// 计算X偏移位置...
		xPos -= vBounds.x;
		// 设置偏移和查找位置...
		vec2_set(&vPos, xPos, 0.0f);
		vec2_set(&vFind, vPos.x + 5, vPos.y + 5);
		lpItemFind = OBSBasicPreview::GetItemAtPos(vFind, false);
		bool bIsFloated = obs_sceneitem_floated(lpItemFind);
		// 查找位置没有数据源或没有浮动，设定位置，跳出循环...
		if (lpItemFind == NULL || !bIsFloated) {
			obs_sceneitem_set_pos(curItem, &vPos);
			break;
		}
	}
	int nCurStep = 0;
	// 计算第一个资源的宽和高...
	ovi.base_height -= DEF_ROW_SPACE;
	uint32_t first_width = ovi.base_width;
	uint32_t first_height = ovi.base_height - ovi.base_height / DEF_ROW_SIZE;
	// 计算其它资源的宽和高...
	ovi.base_width -= (DEF_COL_SIZE - 1) * DEF_COL_SPACE;
	uint32_t other_width = ovi.base_width / DEF_COL_SIZE;
	uint32_t other_height = ovi.base_height / DEF_ROW_SIZE;
	// 由于set_order会重选焦点，需要只保留当前数据源焦点...
	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem * listItem = ui->sources->item(i);
		OBSSceneItem theItem = this->GetSceneItem(listItem);
		bool bIsSelect = ((theItem != curItem) ? false : true);
		obs_sceneitem_select(theItem, bIsSelect);
		// 重排所有数据源，目的是腾出空位 => 排除0点位置和已浮动数据源...
		obs_source_t * source = obs_sceneitem_get_source(theItem);
		uint32_t flags = obs_source_get_output_flags(source);
		bool bIsFloated = obs_sceneitem_floated(theItem);
		// 如果数据源对象无效，继续寻找...
		if (theItem == NULL || source == NULL)
			continue;
		// 如果没有视频数据源标志或处于浮动状态，继续寻找...
		if (bIsFloated || (flags & OBS_SOURCE_VIDEO) == 0)
			continue;
		// 如果数据源是在0点位置，继续寻找...
		if (theItem == m_lpZeroSceneItem)
			continue;
		vec2 vPos = { 1.0f, 1.0f };
		// 遍历第二行的位置，查找空闲位置，即使越界了也要查找...
		uint32_t pos_x = (nCurStep++) * (other_width + DEF_COL_SPACE);
		uint32_t pos_y = first_height + DEF_ROW_SPACE;
		vec2_set(&vPos, float(pos_x), float(pos_y));
		obs_sceneitem_set_pos(theItem, &vPos);
	}
	// 判断是否显示左右箭头...
	this->doCheckBtnPage();
	// 将全部麦克风按钮都重置显示位置...
	ui->preview->ResizeBtnMicAll();
}

void OBSBasic::ShutFloatSource()
{
	// 通过标准接口，查找当前已选中数据源...
	OBSScene scene = this->GetCurrentScene();
	OBSSceneItem curItem = this->GetCurrentSceneItem();
	if (scene == NULL || curItem == NULL)
		return;
	// 先关闭浮动，然后给当前浮动对象寻找放置位置...
	obs_sceneitem_set_floated(curItem, false);
	this->doSceneItemLayout(curItem);
	// 将全部麦克风按钮都重置显示位置...
	ui->preview->ResizeBtnMicAll();
}

void OBSBasic::CreateSourcePopupMenu(QListWidgetItem *item, bool preview)
{
	QMenu popup(this);
	QPointer<QMenu> previewProjector;
	QPointer<QMenu> sourceProjector;

	OBSSceneItem sceneItem = this->GetSceneItem(item);
	obs_source_t *source = obs_sceneitem_get_source(sceneItem);
	bool bIsRtpSource = ((astrcmpi(obs_source_get_id(source), App()->InteractRtpSource()) == 0) ? true : false);

	// 如果数据源有效，创建一个可浮动数据源的菜单项...
	if (item != NULL) {
		QAction * actionFloated = NULL;
		bool bIsFloatedSource = obs_sceneitem_floated(sceneItem);
		actionFloated = popup.addAction(QTStr(bIsFloatedSource ? "Basic.Main.ShutFloatSource" : "Basic.Main.OpenFloatSource"),
										this, bIsFloatedSource ? SLOT(ShutFloatSource()) : SLOT(OpenFloatSource()));
		// 获取坐标位置...
		vec2 posDisp = { 1.0f, 1.0f };
		obs_sceneitem_get_pos(sceneItem, &posDisp);
		// 如果是第一个数据源并且是未浮动状态，需要对菜单进行灰色处理...
		if (posDisp.x <= 0.0f && posDisp.y <= 0.0f && !bIsFloatedSource) {
			actionFloated->setEnabled(false);
		}
		// 增加一个分隔符...
		popup.addSeparator();
	}
	// 针对互动教室，增加打开云台控制菜单...
	if ((source != NULL) && bIsRtpSource) {
		popup.addAction(QTStr("Basic.Main.OpenRtpPTZ"), this, SLOT(OpenWindowPTZ()));
		popup.addSeparator();
	}
	// 针对预览窗口的右键菜单...
	if (preview != NULL) {
		// 屏蔽 开启预览 菜单开关 => 永远开启预览...
		/*QAction *action = popup.addAction(
				QTStr("Basic.Main.PreviewConextMenu.Enable"),
				this, SLOT(TogglePreview()));
		action->setCheckable(true);
		action->setChecked(obs_display_enabled(ui->preview->GetDisplay()));
		if (IsPreviewProgramMode())	action->setEnabled(false);*/

		// 屏蔽 锁定预览和预览缩放 菜单选项...
		//popup.addAction(ui->actionLockPreview);
		//popup.addMenu(ui->scalingMenu);

		// 屏蔽 预览投影菜单，避免与数据源混淆 => 2019.05.20 by jackey...
		/*previewProjector = new QMenu(QTStr("PreviewProjector"));
		AddProjectorMenuMonitors(previewProjector, this,
				SLOT(OpenPreviewProjector()));
		popup.addMenu(previewProjector);

		QAction *previewWindow = popup.addAction(
				QTStr("PreviewWindow"),
				this, SLOT(OpenPreviewWindow()));
		popup.addAction(previewWindow);
		popup.addSeparator();*/
	}

	if (item != NULL) {
		// 开启 滤镜 | 属性 菜单...
		popup.addAction(QTStr("Filters"), this, SLOT(OpenFilters()));
		popup.addAction(QTStr("Properties"), this, SLOT(on_actionSourceProperties_triggered()));
		popup.addSeparator();

		// 添加投影|预览菜单...
		sourceProjector = new QMenu(QTStr("SourceProjector"));
		AddProjectorMenuMonitors(sourceProjector, this, SLOT(OpenSourceProjector()));
		QAction *sourceWindow = popup.addAction(QTStr("SourceWindow"), this, SLOT(OpenSourceWindow()));
		popup.addMenu(sourceProjector);
		popup.addAction(sourceWindow);
		popup.addSeparator();
	}

	QPointer<QMenu> addSourceMenu = CreateAddSourcePopupMenu();
	if (addSourceMenu) popup.addMenu(addSourceMenu);

	//ui->actionCopyFilters->setEnabled(false);
	//ui->actionCopySource->setEnabled(false);

	//popup.addSeparator();
	//popup.addAction(ui->actionCopySource);
	//popup.addAction(ui->actionPasteRef);
	//popup.addAction(ui->actionPasteDup);
	//popup.addSeparator();

	//popup.addSeparator();
	//popup.addAction(ui->actionCopyFilters);
	//popup.addAction(ui->actionPasteFilters);
	//popup.addSeparator();

	if (item != NULL) {
		if (addSourceMenu) popup.addSeparator();
		uint32_t flags = obs_source_get_output_flags(source);
		bool isAsyncVideo = (flags & OBS_SOURCE_ASYNC_VIDEO) == OBS_SOURCE_ASYNC_VIDEO;
		bool hasAudio = (flags & OBS_SOURCE_AUDIO) == OBS_SOURCE_AUDIO;

		popup.addAction(QTStr("RenameSource"), this, SLOT(EditSceneItemName()));
		popup.addAction(QTStr("RemoveSource"), this, SLOT(on_actionRemoveSource_triggered()));
		popup.addSeparator();

		// 加入排序和变换菜单...
		popup.addMenu(ui->orderMenu);
		popup.addMenu(ui->transformMenu);

		if (hasAudio) {
			QAction *actionHideMixer = popup.addAction(
					QTStr("HideMixer"),
					this, SLOT(ToggleHideMixer()));
			actionHideMixer->setCheckable(true);
			actionHideMixer->setChecked(SourceMixerHidden(source));
		}

		if (isAsyncVideo) {
			popup.addMenu(AddDeinterlacingMenu(source));
			popup.addSeparator();
		}

		popup.addMenu(AddScaleFilteringMenu(sceneItem));

		// 屏蔽 交互 菜单...
		/*action = popup.addAction(QTStr("Interact"), this,
				SLOT(on_actionInteract_triggered()));
		action->setEnabled(obs_source_get_output_flags(source) &
				OBS_SOURCE_INTERACTION);*/

		//ui->actionCopyFilters->setEnabled(true);
		//ui->actionCopySource->setEnabled(true);
	} else {
		//ui->actionPasteFilters->setEnabled(false);
	}

	// 始终自动追加一个“检查升级”的菜单...
	if (ui->actionCheckForUpdates != NULL) {
		popup.addSeparator();
		popup.addAction(ui->actionCheckForUpdates);
	}

	popup.exec(QCursor::pos());
}

void OBSBasic::on_sources_customContextMenuRequested(const QPoint &pos)
{
	if (ui->scenes->count())
		CreateSourcePopupMenu(ui->sources->itemAt(pos), false);
}

void OBSBasic::on_sources_itemDoubleClicked(QListWidgetItem *witem)
{
	if (!witem)
		return;

	OBSSceneItem item = GetSceneItem(witem);
	OBSSource source = obs_sceneitem_get_source(item);

	if (source)
		CreatePropertiesWindow(source);
}

void OBSBasic::on_scenes_itemDoubleClicked(QListWidgetItem *witem)
{
	if (!witem)
		return;

	if (IsPreviewProgramMode()) {
		bool doubleClickSwitch = config_get_bool(App()->GlobalConfig(),
				"BasicWindow", "TransitionOnDoubleClick");

		if (doubleClickSwitch) {
			OBSScene scene = GetCurrentScene();

			if (scene)
				SetCurrentScene(scene, false, true);
		}
	}
}

void OBSBasic::AddSource(const char *id)
{
	if (id && *id) {
		bool bIsScreen = false;
		const char * lpNewID = id;
		// 这里需要对slide_screen特殊处理...
		if (strcmp(id, "slide_screen") == 0) {
			lpNewID = "slideshow";
			bIsScreen = true;
		}
		OBSBasicSourceSelect sourceSelect(this, lpNewID, bIsScreen);
		sourceSelect.exec();
		if (sourceSelect.newSource)
			CreatePropertiesWindow(sourceSelect.newSource);
	}
}

QMenu *OBSBasic::CreateAddSourcePopupMenu()
{
	const char *type;
	bool foundValues = false;
	bool foundDeprecated = false;
	size_t idx = 0;

	QMenu *popup = new QMenu(QTStr("AddSource"), this);
	QMenu *deprecated = new QMenu(QTStr("Deprecated"), popup);

	auto getActionAfter = [] (QMenu *menu, const QString &name)
	{
		QList<QAction*> actions = menu->actions();

		for (QAction *menuAction : actions) {
			if (menuAction->text().compare(name) >= 0)
				return menuAction;
		}

		return (QAction*)nullptr;
	};

	auto addSource = [this, getActionAfter] (QMenu *popup,
			const char *type, const char *name)
	{
		QString qname = QT_UTF8(name);
		QAction *popupItem = new QAction(qname, this);
		popupItem->setData(QT_UTF8(type));
		connect(popupItem, SIGNAL(triggered(bool)),
				this, SLOT(AddSourceFromAction()));

		QAction *after = getActionAfter(popup, qname);
		popup->insertAction(after, popupItem);
	};

	while (obs_enum_input_types(idx++, &type)) {
		const char *name = obs_source_get_display_name(type);
		uint32_t caps = obs_get_source_output_flags(type);

		if ((caps & OBS_SOURCE_CAP_DISABLED) != 0)
			continue;

#ifdef _WIN32
		const char *text_source_id = "text_gdiplus";
#else
		const char *text_source_id = "text_ft2_source";
#endif
		// 去掉 文本(GDI+) 的菜单添加入口...
		if (astrcmpi(type, text_source_id) == 0)
			continue;
		// 去掉 图片幻灯片 的菜单添加入口...
		//if (astrcmpi(type, "slideshow") == 0)
		//	continue;
		// 去掉 色源 的菜单添加入口...
		if (astrcmpi(type, "color_source") == 0)
			continue;
		// 去掉 音频输入捕获 的菜单添加入口...
		if (astrcmpi(type, "wasapi_input_capture") == 0)
			continue;
		// 添加相关数据源的菜单入口...
		if ((caps & OBS_SOURCE_DEPRECATED) == 0) {
			addSource(popup, type, name);
		} else {
			addSource(deprecated, type, name);
			foundDeprecated = true;
		}
		foundValues = true;
	}
	// 直接新增 学生屏幕分享 菜单 => 使用一个特殊标志 => AddSource()...
	addSource(popup, "slide_screen", Str("Basic.Student.Screen"));

	// 去掉场景叠加功能，只用数据源的组合功能...
	//addSource(popup, "scene", Str("Basic.Scene"));

	if (!foundDeprecated) {
		delete deprecated;
		deprecated = nullptr;
	}

	if (!foundValues) {
		delete popup;
		popup = nullptr;

	} else if (foundDeprecated) {
		popup->addMenu(deprecated);
	}

	return popup;
}

void OBSBasic::AddSourceFromAction()
{
	QAction *action = qobject_cast<QAction*>(sender());
	if (!action)
		return;

	AddSource(QT_TO_UTF8(action->data().toString()));
}

void OBSBasic::AddSourcePopupMenu(const QPoint &pos)
{
	if (!GetCurrentScene()) {
		// Tell the user he needs a scene first (help beginners).
		OBSMessageBox::information(this,
				QTStr("Basic.Main.AddSourceHelp.Title"),
				QTStr("Basic.Main.AddSourceHelp.Text"));
		return;
	}

	QPointer<QMenu> popup = CreateAddSourcePopupMenu();
	if (popup)
		popup->exec(pos);
}

void OBSBasic::on_actionAddSource_triggered()
{
	AddSourcePopupMenu(QCursor::pos());
}

void OBSBasic::on_actionRemoveSource_triggered()
{
	vector<OBSSceneItem> items;
	auto func = [] (obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		vector<OBSSceneItem> &items =
			*reinterpret_cast<vector<OBSSceneItem>*>(param);
		if (obs_sceneitem_selected(item))
			items.emplace_back(item);
		return true;
	};

	obs_scene_enum_items(GetCurrentScene(), func, &items);

	if (!items.size())
		return;

	auto removeMultiple = [this] (size_t count)
	{
		QString text = QTStr("ConfirmRemove.TextMultiple").arg(QString::number(count));

		QMessageBox remove_items(this);
		remove_items.setText(text);

		QAbstractButton *Yes = remove_items.addButton(QTStr("Yes"),	QMessageBox::YesRole);
		remove_items.addButton(QTStr("No"), QMessageBox::NoRole);
		remove_items.setIcon(QMessageBox::Question);
		remove_items.setWindowTitle(QTStr("ConfirmRemove.Title"));
		remove_items.exec();

		return Yes == remove_items.clickedButton();
	};

	if (items.size() == 1) {
		OBSSceneItem &item = items[0];
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (source && QueryRemoveSource(source)) {
			obs_sceneitem_remove(item);
		}
	} else {
		if (removeMultiple(items.size())) {
			for (auto &item : items) {
				obs_sceneitem_remove(item);
			}
		}
	}
}

void OBSBasic::on_actionInteract_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_source(item);

	if (source)
		CreateInteractionWindow(source);
}

void OBSBasic::on_actionSourceProperties_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_source(item);

	if (source)
		CreatePropertiesWindow(source);
}

void OBSBasic::on_actionSourceUp_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_UP);
}

void OBSBasic::on_actionSourceDown_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_DOWN);
}

void OBSBasic::on_actionMoveUp_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_UP);
}

void OBSBasic::on_actionMoveDown_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_DOWN);
}

void OBSBasic::on_actionMoveToTop_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
}

void OBSBasic::on_actionMoveToBottom_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_BOTTOM);
}

static BPtr<char> ReadLogFile(const char *subdir, const char *log)
{
	char logDir[512];
	if (GetConfigPath(logDir, sizeof(logDir), subdir) <= 0)
		return nullptr;

	string path = (char*)logDir;
	path += "/";
	path += log;

	BPtr<char> file = os_quick_read_utf8_file(path.c_str());
	if (!file)
		blog(LOG_WARNING, "Failed to read log file %s", path.c_str());

	return file;
}

void OBSBasic::UploadLog(const char *subdir, const char *file)
{
	/*BPtr<char> fileString{ReadLogFile(subdir, file)};

	if (!fileString)
		return;

	if (!*fileString)
		return;

	//ui->menuLogFiles->setEnabled(false);

	stringstream ss;
	ss << "OBS " << App()->GetVersionString()
	   << " log file uploaded at " << CurrentDateTimeString()
	   << "\n\n" << fileString;


	if (logUploadThread) {
		logUploadThread->wait();
		delete logUploadThread;
	}

	RemoteTextThread *thread = new RemoteTextThread(
			"https://hastebin.com/documents",
			"text/plain", ss.str().c_str());

	logUploadThread = thread;
	connect(thread, &RemoteTextThread::Result,
			this, &OBSBasic::logUploadFinished);
	logUploadThread->start();*/
}

void OBSBasic::on_actionShowLogs_triggered()
{
	char logDir[512];
	if (GetConfigPath(logDir, sizeof(logDir), "obs-teacher/logs") <= 0)
		return;

	QUrl url = QUrl::fromLocalFile(QT_UTF8(logDir));
	QDesktopServices::openUrl(url);
}

void OBSBasic::on_actionUploadCurrentLog_triggered()
{
	UploadLog("obs-teacher/logs", App()->GetCurrentLog());
}

void OBSBasic::on_actionUploadLastLog_triggered()
{
	UploadLog("obs-teacher/logs", App()->GetLastLog());
}

void OBSBasic::on_actionViewCurrentLog_triggered()
{
	char logDir[512];
	if (GetConfigPath(logDir, sizeof(logDir), "obs-teacher/logs") <= 0)
		return;

	const char* log = App()->GetCurrentLog();

	string path = (char*)logDir;
	path += "/";
	path += log;

	QUrl url = QUrl::fromLocalFile(QT_UTF8(path.c_str()));
	QDesktopServices::openUrl(url);
}

void OBSBasic::on_actionShowCrashLogs_triggered()
{
	char logDir[512];
	if (GetConfigPath(logDir, sizeof(logDir), "obs-teacher/crashes") <= 0)
		return;

	QUrl url = QUrl::fromLocalFile(QT_UTF8(logDir));
	QDesktopServices::openUrl(url);
}

void OBSBasic::on_actionUploadLastCrashLog_triggered()
{
	UploadLog("obs-teacher/crashes", App()->GetLastCrashLog());
}

void OBSBasic::on_actionCheckForUpdates_triggered()
{
	CheckForUpdates(true);
}

void OBSBasic::logUploadFinished(const QString &text, const QString &error)
{
	/*ui->menuLogFiles->setEnabled(true);

	if (text.isEmpty()) {
		OBSMessageBox::information(this,
				QTStr("LogReturnDialog.ErrorUploadingLog"),
				error);
		return;
	}

	obs_data_t *returnData = obs_data_create_from_json(QT_TO_UTF8(text));
	string resURL = "https://hastebin.com/";
	resURL += obs_data_get_string(returnData, "key");
	QString logURL = resURL.c_str();
	obs_data_release(returnData);

	OBSLogReply logDialog(this, logURL);
	logDialog.exec();*/
}

static void RenameListItem(OBSBasic *parent, QListWidget *listWidget,
		obs_source_t *source, const string &name)
{
	const char *prevName = obs_source_get_name(source);
	if (name == prevName)
		return;

	obs_source_t    *foundSource = obs_get_source_by_name(name.c_str());
	QListWidgetItem *listItem    = listWidget->currentItem();

	if (foundSource || name.empty()) {
		listItem->setText(QT_UTF8(prevName));

		if (foundSource) {
			OBSMessageBox::information(parent,
				QTStr("NameExists.Title"),
				QTStr("NameExists.Text"));
		} else if (name.empty()) {
			OBSMessageBox::information(parent,
				QTStr("NoNameEntered.Title"),
				QTStr("NoNameEntered.Text"));
		}

		obs_source_release(foundSource);
	} else {
		listItem->setText(QT_UTF8(name.c_str()));
		obs_source_set_name(source, name.c_str());
	}
}

void OBSBasic::SceneNameEdited(QWidget *editor,
		QAbstractItemDelegate::EndEditHint endHint)
{
	OBSScene  scene = GetCurrentScene();
	QLineEdit *edit = qobject_cast<QLineEdit*>(editor);
	string    text  = QT_TO_UTF8(edit->text().trimmed());

	if (!scene)
		return;

	obs_source_t *source = obs_scene_get_source(scene);
	RenameListItem(this, ui->scenes, source, text);

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED);

	UNUSED_PARAMETER(endHint);
}

void OBSBasic::SceneItemNameEdited(QWidget *editor,
		QAbstractItemDelegate::EndEditHint endHint)
{
	OBSSceneItem item  = GetCurrentSceneItem();
	QLineEdit    *edit = qobject_cast<QLineEdit*>(editor);
	string       text  = QT_TO_UTF8(edit->text().trimmed());

	if (!item)
		return;

	obs_source_t *source = obs_sceneitem_get_source(item);
	RenameListItem(this, ui->sources, source, text);

	// 这里不能用currentItem，如果从预览到达的重命名，有可能当前Item不是选中Item...
	QListWidgetItem *listItem = this->GetTopSelectedSourceItem(); //ui->sources->currentItem();
	listItem->setText(QString());
	SetupVisibilityItem(ui->sources, listItem, item);

	UNUSED_PARAMETER(endHint);
}

void OBSBasic::OpenFilters()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_source(item);

	CreateFiltersWindow(source);
}

void OBSBasic::OpenSceneFilters()
{
	OBSScene scene = GetCurrentScene();
	OBSSource source = obs_scene_get_source(scene);

	CreateFiltersWindow(source);
}

#define RECORDING_START \
	"==== Recording Start ==============================================="
#define RECORDING_STOP \
	"==== Recording Stop ================================================"
#define REPLAY_BUFFER_START \
	"==== Replay Buffer Start ==========================================="
#define REPLAY_BUFFER_STOP \
	"==== Replay Buffer Stop ============================================"
#define STREAMING_START \
	"==== Streaming Start ==============================================="
#define STREAMING_STOP \
	"==== Streaming Stop ================================================"

void OBSBasic::StartStreaming()
{
	if (outputHandler->StreamingActive())
		return;
	if (disableOutputsRef)
		return;

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_STREAMING_STARTING);

	SaveProject();

	ui->streamButton->setEnabled(false);
	ui->streamButton->setText(QTStr("Basic.Main.Connecting"));

	if (sysTrayStream) {
		sysTrayStream->setEnabled(false);
		sysTrayStream->setText(ui->streamButton->text());
	}

	if (!outputHandler->StartStreaming(service)) {
		ui->streamButton->setText(QTStr("Basic.Main.StartStreaming"));
		ui->streamButton->setEnabled(true);
		ui->streamButton->setChecked(false);

		if (sysTrayStream) {
			sysTrayStream->setText(ui->streamButton->text());
			sysTrayStream->setEnabled(true);
		}

		QMessageBox::critical(this,
				QTStr("Output.StartStreamFailed"),
				QTStr("Output.StartFailedGeneric"));
		return;
	}

	bool recordWhenStreaming = config_get_bool(GetGlobalConfig(),
			"BasicWindow", "RecordWhenStreaming");
	if (recordWhenStreaming)
		StartRecording();

	bool replayBufferWhileStreaming = config_get_bool(GetGlobalConfig(),
		"BasicWindow", "ReplayBufferWhileStreaming");
	if (replayBufferWhileStreaming)
		StartReplayBuffer();
}

#ifdef _WIN32
static inline void UpdateProcessPriority()
{
	const char *priority = config_get_string(App()->GlobalConfig(),
			"General", "ProcessPriority");
	if (priority && strcmp(priority, "Normal") != 0)
		SetProcessPriority(priority);
}

static inline void ClearProcessPriority()
{
	const char *priority = config_get_string(App()->GlobalConfig(),
			"General", "ProcessPriority");
	if (priority && strcmp(priority, "Normal") != 0)
		SetProcessPriority("Normal");
}
#else
#define UpdateProcessPriority() do {} while(false)
#define ClearProcessPriority() do {} while(false)
#endif

inline void OBSBasic::OnActivate()
{
	/*if (ui->profileMenu->isEnabled()) {
		ui->profileMenu->setEnabled(false);
		ui->autoConfigure->setEnabled(false);
		App()->IncrementSleepInhibition();
		UpdateProcessPriority();

		if (trayIcon)
			trayIcon->setIcon(QIcon(":/res/images/tray_active.png"));
	}*/
}

inline void OBSBasic::OnDeactivate()
{
	/*if (!outputHandler->Active() && !ui->profileMenu->isEnabled()) {
		ui->profileMenu->setEnabled(true);
		ui->autoConfigure->setEnabled(true);
		App()->DecrementSleepInhibition();
		ClearProcessPriority();

		if (trayIcon)
			trayIcon->setIcon(QIcon(":/res/images/obs.png"));
	}*/
}

void OBSBasic::StopStreaming()
{
	SaveProject();

	if (outputHandler->StreamingActive())
		outputHandler->StopStreaming(streamingStopping);

	OnDeactivate();

	bool recordWhenStreaming = config_get_bool(GetGlobalConfig(),
			"BasicWindow", "RecordWhenStreaming");
	bool keepRecordingWhenStreamStops = config_get_bool(GetGlobalConfig(),
			"BasicWindow", "KeepRecordingWhenStreamStops");
	if (recordWhenStreaming && !keepRecordingWhenStreamStops)
		StopRecording();

	bool replayBufferWhileStreaming = config_get_bool(GetGlobalConfig(),
		"BasicWindow", "ReplayBufferWhileStreaming");
	bool keepReplayBufferStreamStops = config_get_bool(GetGlobalConfig(),
		"BasicWindow", "KeepReplayBufferStreamStops");
	if (replayBufferWhileStreaming && !keepReplayBufferStreamStops)
		StopReplayBuffer();
}

void OBSBasic::ForceStopStreaming()
{
	SaveProject();

	if (outputHandler->StreamingActive())
		outputHandler->StopStreaming(true);

	OnDeactivate();

	bool recordWhenStreaming = config_get_bool(GetGlobalConfig(),
			"BasicWindow", "RecordWhenStreaming");
	bool keepRecordingWhenStreamStops = config_get_bool(GetGlobalConfig(),
			"BasicWindow", "KeepRecordingWhenStreamStops");
	if (recordWhenStreaming && !keepRecordingWhenStreamStops)
		StopRecording();

	bool replayBufferWhileStreaming = config_get_bool(GetGlobalConfig(),
		"BasicWindow", "ReplayBufferWhileStreaming");
	bool keepReplayBufferStreamStops = config_get_bool(GetGlobalConfig(),
		"BasicWindow", "KeepReplayBufferStreamStops");
	if (replayBufferWhileStreaming && !keepReplayBufferStreamStops)
		StopReplayBuffer();
}

void OBSBasic::StreamDelayStarting(int sec)
{
	ui->streamButton->setText(QTStr("Basic.Main.StopStreaming"));
	ui->streamButton->setEnabled(true);
	ui->streamButton->setChecked(true);

	if (sysTrayStream) {
		sysTrayStream->setText(ui->streamButton->text());
		sysTrayStream->setEnabled(true);
	}

	if (!startStreamMenu.isNull())
		startStreamMenu->deleteLater();

	startStreamMenu = new QMenu();
	startStreamMenu->addAction(QTStr("Basic.Main.StopStreaming"),
			this, SLOT(StopStreaming()));
	startStreamMenu->addAction(QTStr("Basic.Main.ForceStopStreaming"),
			this, SLOT(ForceStopStreaming()));
	ui->streamButton->setMenu(startStreamMenu);

	ui->statusbar->StreamDelayStarting(sec);

	OnActivate();
}

void OBSBasic::StreamDelayStopping(int sec)
{
	ui->streamButton->setText(QTStr("Basic.Main.StartStreaming"));
	ui->streamButton->setEnabled(true);
	ui->streamButton->setChecked(false);

	if (sysTrayStream) {
		sysTrayStream->setText(ui->streamButton->text());
		sysTrayStream->setEnabled(true);
	}

	if (!startStreamMenu.isNull())
		startStreamMenu->deleteLater();

	startStreamMenu = new QMenu();
	startStreamMenu->addAction(QTStr("Basic.Main.StartStreaming"),
			this, SLOT(StartStreaming()));
	startStreamMenu->addAction(QTStr("Basic.Main.ForceStopStreaming"),
			this, SLOT(ForceStopStreaming()));
	ui->streamButton->setMenu(startStreamMenu);

	ui->statusbar->StreamDelayStopping(sec);
}

void OBSBasic::StreamingStart()
{
	ui->streamButton->setText(QTStr("Basic.Main.StopStreaming"));
	ui->streamButton->setEnabled(true);
	ui->streamButton->setChecked(true);
	ui->statusbar->StreamStarted(outputHandler->streamOutput);

	if (sysTrayStream) {
		sysTrayStream->setText(ui->streamButton->text());
		sysTrayStream->setEnabled(true);
	}

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_STREAMING_STARTED);

	OnActivate();

	blog(LOG_INFO, STREAMING_START);
}

void OBSBasic::StreamStopping()
{
	ui->streamButton->setText(QTStr("Basic.Main.StoppingStreaming"));

	if (sysTrayStream)
		sysTrayStream->setText(ui->streamButton->text());

	streamingStopping = true;
	if (api)
		api->on_event(OBS_FRONTEND_EVENT_STREAMING_STOPPING);
}

void OBSBasic::StreamingStop(int code, QString last_error)
{
	const char *errorDescription;
	DStr errorMessage;
	bool use_last_error = false;

	switch (code) {
	case OBS_OUTPUT_BAD_PATH:
		errorDescription = Str("Output.ConnectFail.BadPath");
		break;

	case OBS_OUTPUT_CONNECT_FAILED:
		use_last_error = true;
		errorDescription = Str("Output.ConnectFail.ConnectFailed");
		break;

	case OBS_OUTPUT_INVALID_STREAM:
		errorDescription = Str("Output.ConnectFail.InvalidStream");
		break;

	default:
	case OBS_OUTPUT_ERROR:
		use_last_error = true;
		errorDescription = Str("Output.ConnectFail.Error");
		break;

	case OBS_OUTPUT_DISCONNECTED:
		/* doesn't happen if output is set to reconnect.  note that
		 * reconnects are handled in the output, not in the UI */
		use_last_error = true;
		errorDescription = Str("Output.ConnectFail.Disconnected");
	}

	if (use_last_error && !last_error.isEmpty())
		dstr_printf(errorMessage, "%s\n\n%s", errorDescription,
			QT_TO_UTF8(last_error));
	else
		dstr_copy(errorMessage, errorDescription);

	ui->statusbar->StreamStopped();

	ui->streamButton->setText(QTStr("Basic.Main.StartStreaming"));
	ui->streamButton->setEnabled(true);
	ui->streamButton->setChecked(false);

	if (sysTrayStream) {
		sysTrayStream->setText(ui->streamButton->text());
		sysTrayStream->setEnabled(true);
	}

	streamingStopping = false;
	if (api)
		api->on_event(OBS_FRONTEND_EVENT_STREAMING_STOPPED);

	OnDeactivate();

	blog(LOG_INFO, STREAMING_STOP);

	if (code != OBS_OUTPUT_SUCCESS && isVisible()) {
		OBSMessageBox::information(this,
				QTStr("Output.ConnectFail.Title"),
				QT_UTF8(errorMessage));
	} else if (code != OBS_OUTPUT_SUCCESS && !isVisible()) {
		SysTrayNotify(QT_UTF8(errorDescription), QSystemTrayIcon::Warning);
	}

	if (!startStreamMenu.isNull()) {
		ui->streamButton->setMenu(nullptr);
		startStreamMenu->deleteLater();
		startStreamMenu = nullptr;
	}
}

void OBSBasic::doUpdatePTZ(int nDBCameraID)
{
	// 创建云台控制窗口...
	if (m_PTZWindow == NULL) {
		m_PTZWindow = new CPTZWindow(this);
	}
	// 更新摄像头编号到云台控制窗口对象当中...
	if (nDBCameraID > 0 && m_PTZWindow != NULL) {
		m_PTZWindow->doUpdatePTZ(nDBCameraID);
	}
}

bool OBSBasic::doCheckCanRecord()
{
	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem * listItem = ui->sources->item(i);
		OBSSceneItem theItem = this->GetSceneItem(listItem);
		OBSSource theSource = obs_sceneitem_get_source(theItem);
		// 如果资源对象为空，继续寻找...
		if (theSource == NULL)
			continue;
		uint32_t flags = obs_source_get_output_flags(theSource);
		// 如果资源对象只要有一个有视频，就可以录像...
		if (flags & OBS_SOURCE_VIDEO)
			return true;
	}
	// 没有视频资源不可以录像，否则会在ffmpeg-mux当中卡死...
	return false;
}

void OBSBasic::StartRecording()
{
	// 如果没有视频资源，不能录像，否则会在ffmpeg-mux当中卡死...
	if (!this->doCheckCanRecord()) {
		ui->recordButton->setChecked(false);
		return;
	}
	if (outputHandler->RecordingActive())
		return;
	if (disableOutputsRef)
		return;
	if (api) {
		api->on_event(OBS_FRONTEND_EVENT_RECORDING_STARTING);
	}

	SaveProject();
	outputHandler->StartRecording();
}

void OBSBasic::RecordStopping()
{
	ui->recordButton->setText(QTStr("Basic.Main.StoppingRecording"));

	if (sysTrayRecord)
		sysTrayRecord->setText(ui->recordButton->text());

	recordingStopping = true;
	if (api)
		api->on_event(OBS_FRONTEND_EVENT_RECORDING_STOPPING);
}

void OBSBasic::StopRecording()
{
	SaveProject();

	if (outputHandler->RecordingActive())
		outputHandler->StopRecording(recordingStopping);

	OnDeactivate();
}

void OBSBasic::RecordingStart()
{
	ui->statusbar->RecordingStarted(outputHandler->fileOutput);
	ui->recordButton->setText(QTStr("Basic.Main.StopRecording"));
	ui->recordButton->setChecked(true);

	if (sysTrayRecord)
		sysTrayRecord->setText(ui->recordButton->text());

	recordingStopping = false;
	if (api)
		api->on_event(OBS_FRONTEND_EVENT_RECORDING_STARTED);

	OnActivate();

	blog(LOG_INFO, RECORDING_START);
}

void OBSBasic::RecordingStop(int code)
{
	ui->statusbar->RecordingStopped();
	ui->recordButton->setText(QTStr("Basic.Main.StartRecording"));
	ui->recordButton->setChecked(false);

	if (sysTrayRecord)
		sysTrayRecord->setText(ui->recordButton->text());

	blog(LOG_INFO, RECORDING_STOP);

	if (code == OBS_OUTPUT_UNSUPPORTED && isVisible()) {
		OBSMessageBox::information(this,
				QTStr("Output.RecordFail.Title"),
				QTStr("Output.RecordFail.Unsupported"));

	} else if (code == OBS_OUTPUT_NO_SPACE && isVisible()) {
		OBSMessageBox::information(this,
				QTStr("Output.RecordNoSpace.Title"),
				QTStr("Output.RecordNoSpace.Msg"));

	} else if (code != OBS_OUTPUT_SUCCESS && isVisible()) {
		OBSMessageBox::information(this,
				QTStr("Output.RecordError.Title"),
				QTStr("Output.RecordError.Msg"));

	} else if (code == OBS_OUTPUT_UNSUPPORTED && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordFail.Unsupported"),
			QSystemTrayIcon::Warning);

	} else if (code == OBS_OUTPUT_NO_SPACE && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordNoSpace.Msg"),
			QSystemTrayIcon::Warning);

	} else if (code != OBS_OUTPUT_SUCCESS && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordError.Msg"),
			QSystemTrayIcon::Warning);
	}

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_RECORDING_STOPPED);

	OnDeactivate();
}

#define RP_NO_HOTKEY_TITLE QTStr("Output.ReplayBuffer.NoHotkey.Title")
#define RP_NO_HOTKEY_TEXT  QTStr("Output.ReplayBuffer.NoHotkey.Msg")

void OBSBasic::StartReplayBuffer()
{
	if (!outputHandler || !outputHandler->replayBuffer)
		return;
	if (outputHandler->ReplayBufferActive())
		return;
	if (disableOutputsRef)
		return;

	obs_output_t *output = outputHandler->replayBuffer;
	obs_data_t *hotkeys = obs_hotkeys_save_output(output);
	obs_data_array_t *bindings = obs_data_get_array(hotkeys,
			"ReplayBuffer.Save");
	size_t count = obs_data_array_count(bindings);
	obs_data_array_release(bindings);
	obs_data_release(hotkeys);

	if (!count) {
		OBSMessageBox::information(this,
				RP_NO_HOTKEY_TITLE,
				RP_NO_HOTKEY_TEXT);
		return;
	}

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING);

	SaveProject();
	outputHandler->StartReplayBuffer();
}

void OBSBasic::ReplayBufferStopping()
{
	if (!outputHandler || !outputHandler->replayBuffer)
		return;

	replayBufferButton->setText(QTStr("Basic.Main.StoppingReplayBuffer"));

	if (sysTrayReplayBuffer)
		sysTrayReplayBuffer->setText(replayBufferButton->text());

	replayBufferStopping = true;
	if (api)
		api->on_event(OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPING);
}

void OBSBasic::StopReplayBuffer()
{
	if (!outputHandler || !outputHandler->replayBuffer)
		return;

	SaveProject();

	if (outputHandler->ReplayBufferActive())
		outputHandler->StopReplayBuffer(replayBufferStopping);

	OnDeactivate();
}

void OBSBasic::ReplayBufferStart()
{
	if (!outputHandler || !outputHandler->replayBuffer)
		return;

	replayBufferButton->setText(QTStr("Basic.Main.StopReplayBuffer"));

	if (sysTrayReplayBuffer)
		sysTrayReplayBuffer->setText(replayBufferButton->text());

	replayBufferStopping = false;
	if (api)
		api->on_event(OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED);

	OnActivate();

	blog(LOG_INFO, REPLAY_BUFFER_START);
}

void OBSBasic::ReplayBufferSave()
{
	if (!outputHandler || !outputHandler->replayBuffer)
		return;
	if (!outputHandler->ReplayBufferActive())
		return;

	calldata_t cd = {0};
	proc_handler_t *ph = obs_output_get_proc_handler(
			outputHandler->replayBuffer);
	proc_handler_call(ph, "save", &cd);
	calldata_free(&cd);
}

void OBSBasic::ReplayBufferStop(int code)
{
	if (!outputHandler || !outputHandler->replayBuffer)
		return;

	replayBufferButton->setText(QTStr("Basic.Main.StartReplayBuffer"));

	if (sysTrayReplayBuffer)
		sysTrayReplayBuffer->setText(replayBufferButton->text());

	blog(LOG_INFO, REPLAY_BUFFER_STOP);

	if (code == OBS_OUTPUT_UNSUPPORTED && isVisible()) {
		OBSMessageBox::information(this,
				QTStr("Output.RecordFail.Title"),
				QTStr("Output.RecordFail.Unsupported"));

	} else if (code == OBS_OUTPUT_NO_SPACE && isVisible()) {
		OBSMessageBox::information(this,
				QTStr("Output.RecordNoSpace.Title"),
				QTStr("Output.RecordNoSpace.Msg"));

	} else if (code != OBS_OUTPUT_SUCCESS && isVisible()) {
		OBSMessageBox::information(this,
				QTStr("Output.RecordError.Title"),
				QTStr("Output.RecordError.Msg"));

	} else if (code == OBS_OUTPUT_UNSUPPORTED && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordFail.Unsupported"),
			QSystemTrayIcon::Warning);

	} else if (code == OBS_OUTPUT_NO_SPACE && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordNoSpace.Msg"),
			QSystemTrayIcon::Warning);

	} else if (code != OBS_OUTPUT_SUCCESS && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordError.Msg"),
			QSystemTrayIcon::Warning);
	}

	if (api)
		api->on_event(OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED);

	OnDeactivate();
}

void OBSBasic::on_statsButton_clicked()
{
	on_stats_triggered();
}

// 响应学生屏幕分享存盘变化事件更新通知...
void OBSBasic::onTriggerScreenFinish(int nScreenID, QString strQUser, QString strQFile)
{
	// 不能通过唯一的名称查找，有可能会发生改名字的情况...
	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem * lpListItem = ui->sources->item(i);
		OBSSceneItem theSceneItem = this->GetSceneItem(lpListItem);
		obs_source_t * lpFindSource = obs_sceneitem_get_source(theSceneItem);
		const char * id = obs_source_get_id(lpFindSource);
		// 如果当前数据源不是幻灯片，继续查找...
		if (astrcmpi(id, "slideshow") != 0)
			continue;
		// 如果数据源不是 学生屏幕分享 对象，继续查找...
		obs_data_t * lpSettings = obs_source_get_settings(lpFindSource);
		if (!obs_data_get_bool(lpSettings, "screen_slide")) {
			obs_data_release(lpSettings);
			continue;
		}
		// 将最新配置更新到设置当中，并激发更新操作...
		obs_data_set_bool(lpSettings, "screen_change", true);
		obs_data_set_int(lpSettings, "screen_id", nScreenID);
		obs_data_set_string(lpSettings, "screen_user", QT_TO_UTF8(strQUser));
		obs_data_set_string(lpSettings, "screen_file", QT_TO_UTF8(strQFile));
		obs_source_update(lpFindSource, lpSettings);
		obs_data_release(lpSettings);
		break;
	}
}

// 响应服务器发送的rtp_source重建事件通知...
void OBSBasic::onTriggerRtpSource(int nDBCameraID, bool bIsCameraOnLine)
{
	// 通过摄像头编号查找场景对应的数据源...
	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem * lpListItem = ui->sources->item(i);
		OBSSceneItem theSceneItem = this->GetSceneItem(lpListItem);
		obs_source_t * lpFindSource = obs_sceneitem_get_source(theSceneItem);
		obs_data_t * lpSettings = obs_source_get_settings(lpFindSource);
		// 场景配置无效，继续寻找...
		if (lpSettings == NULL)
			continue;
		// 摄像头编号和场景配置不一致，需要释放引用计数器，继续寻找...
		if (nDBCameraID != obs_data_get_int(lpSettings, "camera_id")) {
			obs_data_release(lpSettings);
			continue;
		}
		// 如果云台窗口有效，需要更新...
		this->doUpdatePTZ(nDBCameraID);
		// 将rtp_source需要的参数写入配置结构当中...
		int nRoomID = atoi(App()->GetRoomIDStr().c_str());
		obs_data_set_int(lpSettings, "room_id", nRoomID);
		obs_data_set_int(lpSettings, "camera_id", nDBCameraID);
		obs_data_set_bool(lpSettings, "udp_camera", bIsCameraOnLine);
		obs_data_set_int(lpSettings, "udp_port", App()->GetUdpPort());
		obs_data_set_int(lpSettings, "tcp_socket", App()->GetRtpTCPSockFD());
		obs_data_set_string(lpSettings, "udp_addr", App()->GetUdpAddr().c_str());
		// 将新的资源配置应用到当前rtp_source资源对象当中...
		obs_source_update(lpFindSource, lpSettings);
		// 注意：这里必须手动进行引用计数减少，否则，会造成内存泄漏...
		obs_data_release(lpSettings);
		// 跳出查找循环...
		break;
	}
}

// 响应服务器发送的当前房间在线的摄像头列表事件通知...
void OBSBasic::onTriggerCameraList(Json::Value & value)
{
	if (properties == NULL) return;
	OBSPropertiesView * lpView = properties->GetPropView();
	((lpView != NULL) ? lpView->onTriggerCameraList(value) : NULL);
}

// 响应服务器返回的学生端指定通道停止推流成功的事件通知...
void OBSBasic::onTriggerCameraLiveStop(int nDBCameraID)
{
	if (properties == NULL) return;
	OBSPropertiesView * lpView = properties->GetPropView();
	((lpView != NULL) ? lpView->onTriggerCameraLiveStop(nDBCameraID) : NULL);
	// 响应完毕之后，关闭属性配置窗口...
	properties->doRtpStopClose();
}

// 响应服务器发送的UDP连接对象被删除的事件通知...
void OBSBasic::onTriggerUdpLogout(int tmTag, int idTag, int nDBCameraID)
{
	// 如果不是讲师端对象 => 直接返回...
	if (tmTag != TM_TAG_TEACHER)
		return;
	// 如果是讲师推流端的删除通知...
	if (idTag == ID_TAG_PUSHER) {
		// 输出对象有效，输出流处于活动状态 => 直接停止推流连接...
		if (outputHandler && outputHandler->StreamingActive()) {
			this->StopStreaming();
		}
		return;
	}
	// 如果是讲师观看端的删除通知 => 通过摄像头通道号查找...
	if (idTag == ID_TAG_LOOKER) {
	}
}

// 点击“开始推流”或“停止推流”的按钮事件...
void OBSBasic::on_streamButton_clicked()
{
	if (outputHandler->StreamingActive()) {
		bool confirm = config_get_bool(GetGlobalConfig(), "BasicWindow",
				"WarnBeforeStoppingStream");

		if (confirm && isVisible()) {
			QMessageBox::StandardButton button =
				OBSMessageBox::question(this,
						QTStr("ConfirmStop.Title"),
						QTStr("ConfirmStop.Text"));

			if (button == QMessageBox::No)
				return;
		}

		this->StopStreaming();
	} else {
		bool confirm = config_get_bool(GetGlobalConfig(), "BasicWindow",
				"WarnBeforeStartingStream");

		if (confirm && isVisible()) {
			QMessageBox::StandardButton button =
				OBSMessageBox::question(this,
						QTStr("ConfirmStart.Title"),
						QTStr("ConfirmStart.Text"));

			if (button == QMessageBox::No)
				return;
		}

		this->StartStreaming();
	}
}

void OBSBasic::on_recordButton_clicked()
{
	if (outputHandler->RecordingActive())
		StopRecording();
	else
		StartRecording();
}

void OBSBasic::on_settingsButton_clicked()
{
	on_action_Settings_triggered();
}

void OBSBasic::on_actionHelpPortal_triggered()
{
	QUrl url = QUrl("https://obsproject.com/help", QUrl::TolerantMode);
	QDesktopServices::openUrl(url);
}

void OBSBasic::on_actionWebsite_triggered()
{
	QUrl url = QUrl("https://obsproject.com", QUrl::TolerantMode);
	QDesktopServices::openUrl(url);
}

void OBSBasic::on_actionShowSettingsFolder_triggered()
{
	char path[512];
	int ret = GetConfigPath(path, 512, "obs-teacher");
	if (ret <= 0)
		return;

	QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void OBSBasic::on_actionShowProfileFolder_triggered()
{
	char path[512];
	int ret = GetProfilePath(path, 512, "");
	if (ret <= 0)
		return;

	QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

QListWidgetItem *OBSBasic::GetTopSelectedSourceItem()
{
	QList<QListWidgetItem*> selectedItems = ui->sources->selectedItems();
	QListWidgetItem *topItem = nullptr;
	if (selectedItems.size() != 0)
		topItem = selectedItems[0];
	return topItem;
}

void OBSBasic::on_preview_customContextMenuRequested(const QPoint &pos)
{
	CreateSourcePopupMenu(GetTopSelectedSourceItem(), true);

	UNUSED_PARAMETER(pos);
}

void OBSBasic::on_program_customContextMenuRequested(const QPoint&)
{
	QMenu popup(this);
	QPointer<QMenu> studioProgramProjector;

	studioProgramProjector = new QMenu(
			QTStr("StudioProgramProjector"));
	AddProjectorMenuMonitors(studioProgramProjector, this,
			SLOT(OpenStudioProgramProjector()));

	popup.addMenu(studioProgramProjector);

	QAction *studioProgramWindow = popup.addAction(
			QTStr("StudioProgramWindow"),
			this, SLOT(OpenStudioProgramWindow()));

	popup.addAction(studioProgramWindow);

	popup.exec(QCursor::pos());
}

void OBSBasic::on_previewDisabledLabel_customContextMenuRequested(
		const QPoint &pos)
{
	QMenu popup(this);
	QPointer<QMenu> previewProjector;

	QAction *action = popup.addAction(
			QTStr("Basic.Main.PreviewConextMenu.Enable"),
			this, SLOT(TogglePreview()));
	action->setCheckable(true);
	action->setChecked(obs_display_enabled(ui->preview->GetDisplay()));

	previewProjector = new QMenu(QTStr("PreviewProjector"));
	AddProjectorMenuMonitors(previewProjector, this,
			SLOT(OpenPreviewProjector()));

	QAction *previewWindow = popup.addAction(
			QTStr("PreviewWindow"),
			this, SLOT(OpenPreviewWindow()));

	popup.addMenu(previewProjector);
	popup.addAction(previewWindow);
	popup.exec(QCursor::pos());

	UNUSED_PARAMETER(pos);
}

void OBSBasic::on_actionAlwaysOnTop_triggered()
{
#ifndef _WIN32
	/* Make sure all dialogs are safely and successfully closed before
	 * switching the always on top mode due to the fact that windows all
	 * have to be recreated, so queue the actual toggle to happen after
	 * all events related to closing the dialogs have finished */
	CloseDialogs();
#endif

	QMetaObject::invokeMethod(this, "ToggleAlwaysOnTop", Qt::QueuedConnection);
}

void OBSBasic::ToggleAlwaysOnTop()
{
	bool isAlwaysOnTop = IsAlwaysOnTop(this);

	ui->actionAlwaysOnTop->setChecked(!isAlwaysOnTop);
	SetAlwaysOnTop(this, !isAlwaysOnTop);

	show();
}

void OBSBasic::GetFPSCommon(uint32_t &num, uint32_t &den) const
{
	const char *val = config_get_string(basicConfig, "Video", "FPSCommon");

	if (strcmp(val, "10") == 0) {
		num = 10;
		den = 1;
	} else if (strcmp(val, "20") == 0) {
		num = 20;
		den = 1;
	} else if (strcmp(val, "24 NTSC") == 0) {
		num = 24000;
		den = 1001;
	} else if (strcmp(val, "25") == 0) {
		num = 25;
		den = 1;
	} else if (strcmp(val, "29.97") == 0) {
		num = 30000;
		den = 1001;
	} else if (strcmp(val, "48") == 0) {
		num = 48;
		den = 1;
	} else if (strcmp(val, "59.94") == 0) {
		num = 60000;
		den = 1001;
	} else if (strcmp(val, "60") == 0) {
		num = 60;
		den = 1;
	} else {
		num = 30;
		den = 1;
	}
}

void OBSBasic::GetFPSInteger(uint32_t &num, uint32_t &den) const
{
	num = (uint32_t)config_get_uint(basicConfig, "Video", "FPSInt");
	den = 1;
}

void OBSBasic::GetFPSFraction(uint32_t &num, uint32_t &den) const
{
	num = (uint32_t)config_get_uint(basicConfig, "Video", "FPSNum");
	den = (uint32_t)config_get_uint(basicConfig, "Video", "FPSDen");
}

void OBSBasic::GetFPSNanoseconds(uint32_t &num, uint32_t &den) const
{
	num = 1000000000;
	den = (uint32_t)config_get_uint(basicConfig, "Video", "FPSNS");
}

void OBSBasic::GetConfigFPS(uint32_t &num, uint32_t &den) const
{
	uint32_t type = config_get_uint(basicConfig, "Video", "FPSType");

	if (type == 1) //"Integer"
		GetFPSInteger(num, den);
	else if (type == 2) //"Fraction"
		GetFPSFraction(num, den);
	else if (false) //"Nanoseconds", currently not implemented
		GetFPSNanoseconds(num, den);
	else
		GetFPSCommon(num, den);
}

config_t *OBSBasic::Config() const
{
	return basicConfig;
}

void OBSBasic::on_actionEditTransform_triggered()
{
	if (transformWindow)
		transformWindow->close();

	transformWindow = new OBSBasicTransform(this);
	transformWindow->show();
	transformWindow->setAttribute(Qt::WA_DeleteOnClose, true);
}

static obs_transform_info copiedTransformInfo;
static obs_sceneitem_crop copiedCropInfo;

void OBSBasic::on_actionCopyTransform_triggered()
{
	auto func = [](obs_scene_t *scene, obs_sceneitem_t *item, void *param)
	{
		if (!obs_sceneitem_selected(item))
			return true;

		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_get_info(item, &copiedTransformInfo);
		obs_sceneitem_get_crop(item, &copiedCropInfo);
		obs_sceneitem_defer_update_end(item);

		UNUSED_PARAMETER(scene);
		UNUSED_PARAMETER(param);
		return true;
	};

	obs_scene_enum_items(GetCurrentScene(), func, nullptr);
	ui->actionPasteTransform->setEnabled(true);
}

void OBSBasic::on_actionPasteTransform_triggered()
{
	auto func = [](obs_scene_t *scene, obs_sceneitem_t *item, void *param)
	{
		if (!obs_sceneitem_selected(item))
			return true;

		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_info(item, &copiedTransformInfo);
		obs_sceneitem_set_crop(item, &copiedCropInfo);
		obs_sceneitem_defer_update_end(item);

		UNUSED_PARAMETER(scene);
		UNUSED_PARAMETER(param);
		return true;
	};

	obs_scene_enum_items(GetCurrentScene(), func, nullptr);
}

void OBSBasic::on_actionResetTransform_triggered()
{
	auto func = [] (obs_scene_t *scene, obs_sceneitem_t *item, void *param)
	{
		if (!obs_sceneitem_selected(item))
			return true;

		obs_sceneitem_defer_update_begin(item);

		obs_transform_info info;
		vec2_set(&info.pos, 0.0f, 0.0f);
		vec2_set(&info.scale, 1.0f, 1.0f);
		info.rot = 0.0f;
		info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
		info.bounds_type = OBS_BOUNDS_NONE;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		vec2_set(&info.bounds, 0.0f, 0.0f);
		obs_sceneitem_set_info(item, &info);

		obs_sceneitem_crop crop = {};
		obs_sceneitem_set_crop(item, &crop);

		obs_sceneitem_defer_update_end(item);

		UNUSED_PARAMETER(scene);
		UNUSED_PARAMETER(param);
		return true;
	};

	obs_scene_enum_items(GetCurrentScene(), func, nullptr);
}

static void GetItemBox(obs_sceneitem_t *item, vec3 &tl, vec3 &br)
{
	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	vec3_set(&tl, M_INFINITE, M_INFINITE, 0.0f);
	vec3_set(&br, -M_INFINITE, -M_INFINITE, 0.0f);

	auto GetMinPos = [&] (float x, float y)
	{
		vec3 pos;
		vec3_set(&pos, x, y, 0.0f);
		vec3_transform(&pos, &pos, &boxTransform);
		vec3_min(&tl, &tl, &pos);
		vec3_max(&br, &br, &pos);
	};

	GetMinPos(0.0f, 0.0f);
	GetMinPos(1.0f, 0.0f);
	GetMinPos(0.0f, 1.0f);
	GetMinPos(1.0f, 1.0f);
}

static vec3 GetItemTL(obs_sceneitem_t *item)
{
	vec3 tl, br;
	GetItemBox(item, tl, br);
	return tl;
}

static void SetItemTL(obs_sceneitem_t *item, const vec3 &tl)
{
	vec3 newTL;
	vec2 pos;

	obs_sceneitem_get_pos(item, &pos);
	newTL = GetItemTL(item);
	pos.x += tl.x - newTL.x;
	pos.y += tl.y - newTL.y;
	obs_sceneitem_set_pos(item, &pos);
}

static bool RotateSelectedSources(obs_scene_t *scene, obs_sceneitem_t *item,
		void *param)
{
	if (!obs_sceneitem_selected(item))
		return true;

	float rot = *reinterpret_cast<float*>(param);

	vec3 tl = GetItemTL(item);

	rot += obs_sceneitem_get_rot(item);
	if (rot >= 360.0f)       rot -= 360.0f;
	else if (rot <= -360.0f) rot += 360.0f;
	obs_sceneitem_set_rot(item, rot);

	SetItemTL(item, tl);

	UNUSED_PARAMETER(scene);
	UNUSED_PARAMETER(param);
	return true;
};

void OBSBasic::on_actionRotate90CW_triggered()
{
	float f90CW = 90.0f;
	obs_scene_enum_items(GetCurrentScene(), RotateSelectedSources, &f90CW);
}

void OBSBasic::on_actionRotate90CCW_triggered()
{
	float f90CCW = -90.0f;
	obs_scene_enum_items(GetCurrentScene(), RotateSelectedSources, &f90CCW);
}

void OBSBasic::on_actionRotate180_triggered()
{
	float f180 = 180.0f;
	obs_scene_enum_items(GetCurrentScene(), RotateSelectedSources, &f180);
}

static bool MultiplySelectedItemScale(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	vec2 &mul = *reinterpret_cast<vec2*>(param);

	if (!obs_sceneitem_selected(item))
		return true;

	vec3 tl = GetItemTL(item);

	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	vec2_mul(&scale, &scale, &mul);
	obs_sceneitem_set_scale(item, &scale);

	SetItemTL(item, tl);

	UNUSED_PARAMETER(scene);
	return true;
}

void OBSBasic::on_actionFlipHorizontal_triggered()
{
	vec2 scale;
	vec2_set(&scale, -1.0f, 1.0f);
	obs_scene_enum_items(GetCurrentScene(), MultiplySelectedItemScale, &scale);
}

void OBSBasic::on_actionFlipVertical_triggered()
{
	vec2 scale;
	vec2_set(&scale, 1.0f, -1.0f);
	obs_scene_enum_items(GetCurrentScene(), MultiplySelectedItemScale, &scale);
}

static bool CenterAlignSelectedItems(obs_scene_t *scene, obs_sceneitem_t *item,
		void *param)
{
	obs_bounds_type boundsType = *reinterpret_cast<obs_bounds_type*>(param);

	if (!obs_sceneitem_selected(item))
		return true;

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	obs_transform_info itemInfo;
	vec2_set(&itemInfo.pos, 0.0f, 0.0f);
	vec2_set(&itemInfo.scale, 1.0f, 1.0f);
	itemInfo.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	itemInfo.rot = 0.0f;

	vec2_set(&itemInfo.bounds,
			float(ovi.base_width), float(ovi.base_height));
	itemInfo.bounds_type = boundsType;
	itemInfo.bounds_alignment = OBS_ALIGN_CENTER;

	obs_sceneitem_set_info(item, &itemInfo);

	UNUSED_PARAMETER(scene);
	return true;
}

void OBSBasic::on_actionFitToScreen_triggered()
{
	obs_bounds_type boundsType = OBS_BOUNDS_SCALE_INNER;
	obs_scene_enum_items(GetCurrentScene(), CenterAlignSelectedItems,
			&boundsType);
}

void OBSBasic::on_actionStretchToScreen_triggered()
{
	obs_bounds_type boundsType = OBS_BOUNDS_STRETCH;
	obs_scene_enum_items(GetCurrentScene(), CenterAlignSelectedItems,
			&boundsType);
}

void OBSBasic::on_actionCenterToScreen_triggered()
{
	auto func = [] (obs_scene_t *scene, obs_sceneitem_t *item, void *param)
	{
		vec3 tl, br, itemCenter, screenCenter, offset;
		obs_video_info ovi;

		if (!obs_sceneitem_selected(item))
			return true;

		obs_get_video_info(&ovi);

		vec3_set(&screenCenter, float(ovi.base_width),
				float(ovi.base_height), 0.0f);
		vec3_mulf(&screenCenter, &screenCenter, 0.5f);

		GetItemBox(item, tl, br);

		vec3_sub(&itemCenter, &br, &tl);
		vec3_mulf(&itemCenter, &itemCenter, 0.5f);
		vec3_add(&itemCenter, &itemCenter, &tl);

		vec3_sub(&offset, &screenCenter, &itemCenter);
		vec3_add(&tl, &tl, &offset);

		SetItemTL(item, tl);

		UNUSED_PARAMETER(scene);
		UNUSED_PARAMETER(param);
		return true;
	};

	obs_scene_enum_items(GetCurrentScene(), func, nullptr);
}

void OBSBasic::EnablePreviewDisplay(bool enable)
{
	obs_display_set_enabled(ui->preview->GetDisplay(), enable);
	ui->preview->setVisible(enable);
	ui->previewDisabledLabel->setVisible(!enable);
}

void OBSBasic::TogglePreview()
{
	previewEnabled = !previewEnabled;
	EnablePreviewDisplay(previewEnabled);
}

void OBSBasic::Nudge(int dist, MoveDir dir)
{
	if (ui->preview->Locked())
		return;

	struct MoveInfo {
		float dist;
		MoveDir dir;
	} info = {(float)dist, dir};

	auto func = [] (obs_scene_t*, obs_sceneitem_t *item, void *param)
	{
		if (obs_sceneitem_locked(item))
			return true;

		MoveInfo *info = reinterpret_cast<MoveInfo*>(param);
		struct vec2 dir;
		struct vec2 pos;

		vec2_set(&dir, 0.0f, 0.0f);

		if (!obs_sceneitem_selected(item))
			return true;

		switch (info->dir) {
		case MoveDir::Up:    dir.y = -info->dist; break;
		case MoveDir::Down:  dir.y =  info->dist; break;
		case MoveDir::Left:  dir.x = -info->dist; break;
		case MoveDir::Right: dir.x =  info->dist; break;
		}

		obs_sceneitem_get_pos(item, &pos);
		vec2_add(&pos, &pos, &dir);
		obs_sceneitem_set_pos(item, &pos);
		return true;
	};

	obs_scene_enum_items(GetCurrentScene(), func, &info);
}

void OBSBasic::NudgeUp()       {Nudge(1,  MoveDir::Up);}
void OBSBasic::NudgeDown()     {Nudge(1,  MoveDir::Down);}
void OBSBasic::NudgeLeft()     {Nudge(1,  MoveDir::Left);}
void OBSBasic::NudgeRight()    {Nudge(1,  MoveDir::Right);}

OBSProjector *OBSBasic::OpenProjector(obs_source_t *source, int monitor,
		QString title, ProjectorType type)
{
	/* seriously?  10 monitors? */
	if (monitor > 9 || monitor > QGuiApplication::screens().size() - 1)
		return nullptr;

	OBSProjector *projector = new OBSProjector(nullptr, source, monitor,
			title, type);

	if (monitor < 0) {
		for (auto &projPtr : windowProjectors) {
			if (!projPtr) {
				projPtr = projector;
				projector = nullptr;
			}
		}

		if (projector) {
			windowProjectors.push_back(projector);
		} else {
			// 将现有的窗口赋给显示对象...
			if (windowProjectors.length() > 0) {
				projector = static_cast<OBSProjector *>(windowProjectors.front().data());
			}
		}
	} else {
		delete projectors[monitor];
		projectors[monitor].clear();

		projectors[monitor] = projector;
	}
	// 需要进行有效性判断...
	if( projector != NULL ) {
		projector->Init();
	}
	return projector;
}

// mixerIdx => 指的是轨道编号，0~5总共有6个音频轨道，通过32位整型数字的比特位来记录是否进行混音标志...
static inline void setAudioMixer(obs_sceneitem_t *scene_item, const int mixerIdx, bool enabled)
{
	obs_source_t *source = obs_sceneitem_get_source(scene_item);
	uint32_t mixers = obs_source_get_audio_mixers(source);
	uint32_t new_mixers = mixers;

	// 没有音频属性，或资源为空，直接返回...
	if (mixers <= 0 || source == NULL)
		return;

	/*// 如果是第一个轨道，需要将混音标志存入配置，wasapi-output.c当中会用到...
	// 采用了新的第三轨道混音输出模式，不用第一轨道输出模式了...
	obs_data_t * lpSettings = obs_source_get_settings(source);
	obs_data_set_bool(lpSettings, "focus_mix", enabled);
	obs_data_release(lpSettings);*/
	
	const char * lpSrcID = obs_source_get_id(source);
	bool bIsRtpSource = ((astrcmpi(lpSrcID, App()->InteractRtpSource()) == 0) ? true : false);

	// 注意：如果是互动教室资源、第二个轨道，用来录像的音频，不用处理，默认始终录像...
	if (bIsRtpSource && (mixerIdx == 1))
		return;

	// 注意：如果是互动教室资源、第一个轨道、投递数据，三个条件都满足，就进行强制屏蔽...
	if (bIsRtpSource && enabled && (mixerIdx == 0)) {
		enabled = false;
	}

	if (enabled) new_mixers |= (1 << mixerIdx);
	else         new_mixers &= ~(1 << mixerIdx);

	obs_source_set_audio_mixers(source, new_mixers);
}

// 响应鼠标双击事件 => 交换0点位置的场景资源...
void OBSBasic::doSceneItemExchangePos(obs_sceneitem_t * select_item)
{
	// 输入场景资源为空，直接返回...
	if (select_item == NULL)
		return;
	// 0点无效，将当前设定为0点...
	if (m_lpZeroSceneItem == NULL) {
		// 设定选中对象为0点数据源对象...
		this->doSceneItemToFirst(select_item);
		// 保存当前数据源为0点数据源...
		m_lpZeroSceneItem = select_item;
		return;
	}
	// 0点数据源必须一定不为空...
	ASSERT(m_lpZeroSceneItem != NULL);
	// 获取当前选中资源的坐标信息...
	obs_transform_info selectInfo = { 0 };
	obs_sceneitem_get_info(select_item, &selectInfo);
	// 获取第一个资源对象的全部坐标信息 => 0点数据源对象...
	obs_transform_info firstInfo = { 0 };
	obs_sceneitem_t  * lpFirstSceneItem = m_lpZeroSceneItem;
	obs_sceneitem_get_info(lpFirstSceneItem, &firstInfo);
	ASSERT(firstInfo.pos.x <= 0.0f && firstInfo.pos.y <= 0.0f);
	// 将当前选中资源的坐标信息与第一个资源的坐标信息进行交换...
	obs_sceneitem_set_pos(lpFirstSceneItem, &selectInfo.pos);
	obs_sceneitem_set_bounds(lpFirstSceneItem, &selectInfo.bounds);
	obs_sceneitem_set_pos(select_item, &firstInfo.pos);
	obs_sceneitem_set_bounds(select_item, &firstInfo.bounds);
	// 注意：不能交换全部信息，只交换坐标位置和图像高宽...
	//obs_sceneitem_set_info(lpFirstSceneItem, &selectInfo);
	//obs_sceneitem_set_info(select_item, &firstInfo);
	// 轨道1|轨道2 => 屏蔽旧的第一个窗口音频输出...
	setAudioMixer(lpFirstSceneItem, 0, false);
	setAudioMixer(lpFirstSceneItem, 1, false);
	// 轨道1|轨道2 => 强制新的第一个窗口音频输出...
	setAudioMixer(select_item, 0, true);
	setAudioMixer(select_item, 1, true);
	// 强制新的数据源置于0点位置的最底层，避免遮挡浮动窗口...
	obs_sceneitem_set_order(select_item, OBS_ORDER_MOVE_BOTTOM);
	// 由于set_order会重选焦点，需要只保留当前数据源焦点...
	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem * listItem = ui->sources->item(i);
		OBSSceneItem theItem = this->GetSceneItem(listItem);
		bool bIsSelect = ((theItem != select_item) ? false : true);
		obs_sceneitem_select(theItem, bIsSelect);
	}
	// 0点位置如果是幻灯片数据源，需要显示上下页按钮...
	obs_source_t * lpSource = obs_sceneitem_get_source(select_item);
	this->doCheckPPTSource(lpSource);
	// 保存当前数据源为0点数据源...
	m_lpZeroSceneItem = select_item;
}

// 对场景资源位置进行重新排列 => 两行（1行1列，1行5列）...
void OBSBasic::doSceneItemLayout(obs_sceneitem_t * scene_item)
{
	// 如果数据源无效或没有视频内容，直接返回...
	if (scene_item == NULL)
		return;
	obs_source_t *source = obs_sceneitem_get_source(scene_item);
	uint32_t flags = obs_source_get_output_flags(source);
	// 如果场景资源，没有视频，不重排位置，直接返回...
	if ((flags & OBS_SOURCE_VIDEO) == 0)
		return;
	// 场景数据资源必须包含视频内容...
	ASSERT(flags & OBS_SOURCE_VIDEO);
	// 如果0点无效，将当前设定为0点...
	if (m_lpZeroSceneItem == NULL) {
		// 设定选中对象为0点数据源对象...
		this->doSceneItemToFirst(scene_item);
		// 学生监听状态由讲师手动控制，不再由焦点控制...
		//this->doSendCameraPusherID(scene_item);
		// 更新场景资源的显示位置信息...
		m_lpZeroSceneItem = scene_item;
		return;
	}
	// 获取显示系统的宽和高...
	obs_video_info ovi = { 0 };
	obs_get_video_info(&ovi);
	// 计算第一个资源的宽和高...
	ovi.base_height -= DEF_ROW_SPACE;
	uint32_t first_width = ovi.base_width;
	uint32_t first_height = ovi.base_height - ovi.base_height / DEF_ROW_SIZE;
	// 计算其它资源的宽和高...
	ovi.base_width -= (DEF_COL_SIZE - 1) * DEF_COL_SPACE;
	uint32_t other_width = ovi.base_width / DEF_COL_SIZE;
	uint32_t other_height = ovi.base_height / DEF_ROW_SIZE;
	// 设置统一默认的数据源对齐参数...
	obs_transform_info itemInfo = { 0 };
	obs_sceneitem_get_info(scene_item, &itemInfo);
	itemInfo.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	itemInfo.bounds_type = OBS_BOUNDS_SCALE_INNER;
	itemInfo.bounds_alignment = OBS_ALIGN_CENTER;
	int nCurStep = 0, nItemStep = 0;
	// 重排所有数据源(排除0点位置和已浮动数据源)...
	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem * lpListItem = ui->sources->item(i);
		obs_sceneitem_t * lpCurItem = this->GetSceneItem(lpListItem);
		obs_source_t * lpCurSource = obs_sceneitem_get_source(lpCurItem);
		uint32_t nCurflags = obs_source_get_output_flags(lpCurSource);
		bool bIsFloated = obs_sceneitem_floated(lpCurItem);
		// 如果数据源对象无效，继续寻找...
		if (lpCurItem == NULL || lpCurSource == NULL)
			continue;
		// 如果没有视频数据源标志或处于浮动状态，继续寻找...
		if (bIsFloated || (nCurflags & OBS_SOURCE_VIDEO) == 0)
			continue;
		// 如果数据源就在0点位置，继续寻找...
		if (m_lpZeroSceneItem == lpCurItem)
			continue;
		// 遍历第二行的位置，查找空闲位置，即使越界了也要查找...
		uint32_t pos_x = (nCurStep++) * (other_width + DEF_COL_SPACE);
		uint32_t pos_y = first_height + DEF_ROW_SPACE;
		// 当前位置等于输入数据源，调整输入数据源的位置信息...
		if (lpCurItem == scene_item) {
			// 当前位置没有数据源，更新数据源位置信息...
			vec2_set(&itemInfo.pos, float(pos_x), float(pos_y));
			vec2_set(&itemInfo.bounds, float(other_width), float(other_height));
			obs_sceneitem_set_info(scene_item, &itemInfo);
			// 轨道1|轨道2 => 屏蔽非第一个窗口资源的音频输出，只保留全局音频资源和第一个窗口的音频资源输出...
			setAudioMixer(scene_item, 0, false);
			setAudioMixer(scene_item, 1, false);
			nItemStep = nCurStep;
		} else {
			vec2 vPos = { 0.0f, 0.0f };
			vec2_set(&vPos, float(pos_x), float(pos_y));
			obs_sceneitem_set_pos(lpCurItem, &vPos);
		}
	}
	// 计算所有的数据源需要向左移动的步数...
	if (nItemStep > DEF_COL_SIZE) {
		int nMoveLeftX = (nItemStep - DEF_COL_SIZE) * (other_width + DEF_COL_SPACE);
		this->doMovePageX(nMoveLeftX);
	}
	// 判断是否显示左右箭头...
	this->doCheckBtnPage();
}

// PPT数据源向上翻页...
void OBSBasic::onPagePrevClicked()
{
	this->doBtnPrevNext(true);
}

// PPT数据源向下翻页...
void OBSBasic::onPageNextClicked()
{
	this->doBtnPrevNext(false);
}

// 具体执行上部前后翻页操作的接口函数...
void OBSBasic::doBtnPrevNext(bool bIsPrev)
{
	// 如果0点数据源无效，直接返回...
	if (m_lpZeroSceneItem == NULL)
		return;
	obs_source_t * lpSource = obs_sceneitem_get_source(m_lpZeroSceneItem);
	const char * lpSrcID = obs_source_get_id(lpSource);
	// 如果0点数据源不是PPT文件，直接返回...
	if (astrcmpi(lpSrcID, "slideshow") != 0)
		return;
	// 获取到幻灯片数据源的上一页和下一页的快捷热键值...
	obs_data_t * lpSettings = obs_source_get_settings(lpSource);
	obs_hotkey_id next_hotkey = obs_data_get_int(lpSettings, "next_hotkey");
	obs_hotkey_id prev_hotkey = obs_data_get_int(lpSettings, "prev_hotkey");
	obs_data_release(lpSettings);
	// 根据具体的输入参数标志，得到具体的点击热键值...
	obs_hotkey_id click_hotkey = bIsPrev ? prev_hotkey : next_hotkey;
	// 直接调用幻灯片数据源的热键回调接口 => 激发翻页操作...
	obs_hotkey_trigger_routed_callback(click_hotkey, true);
	// 重新读取被幻灯片改变后的配置...
	lpSettings = obs_source_get_settings(lpSource);
	const char * lpName = obs_data_get_string(lpSettings, "item_name");
	int nCurItem = obs_data_get_int(lpSettings, "cur_item");
	int nFileNum = obs_data_get_int(lpSettings, "file_num");
	obs_data_release(lpSettings);
	// 将读取到的幻灯片数据，直接更新到预览界面当中...
	ui->preview->DispBtnFoot(true, nCurItem, nFileNum, lpName);
}

// 所有第二行数据源窗口向右移动一格...
void OBSBasic::onPageLeftClicked()
{
	this->doBtnLeftRight(true);
}

// 所有第二行数据源窗口向左移动一格...
void OBSBasic::onPageRightClicked()
{
	this->doBtnLeftRight(false);
}

// 具体执行底部左右翻页操作的接口函数...
void OBSBasic::doBtnLeftRight(bool bIsLeft)
{
	// 获取显示系统的宽和高...
	obs_video_info ovi = { 0 };
	obs_get_video_info(&ovi);
	// 计算第一个资源的宽和高...
	ovi.base_height -= DEF_ROW_SPACE;
	uint32_t first_width = ovi.base_width;
	uint32_t first_height = ovi.base_height - ovi.base_height / DEF_ROW_SIZE;
	// 计算其它资源的宽和高...
	ovi.base_width -= (DEF_COL_SIZE - 1) * DEF_COL_SPACE;
	uint32_t other_width = ovi.base_width / DEF_COL_SIZE;
	uint32_t other_height = ovi.base_height / DEF_ROW_SIZE;
	// 计算所有的数据源需要向右移动的步数 => 注意是负数...
	int nMoveLeftX = (bIsLeft ? -1 : 1) * (other_width + DEF_COL_SPACE);
	// 所有第二行数据源窗口向右移动...
	this->doMovePageX(nMoveLeftX);
	// 判断是否显示左右箭头...
	this->doCheckBtnPage();
}

// 遍历所有的数据源，排除0点位置数据源和浮动数据源...
void OBSBasic::doMovePageX(int nMoveLeftX)
{
	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem * listItem = ui->sources->item(i);
		obs_sceneitem_t * item = this->GetSceneItem(listItem);
		obs_source_t * source = obs_sceneitem_get_source(item);
		uint32_t flags = obs_source_get_output_flags(source);
		bool bIsFloated = obs_sceneitem_floated(item);
		// 如果数据源对象无效，继续寻找...
		if (item == NULL || source == NULL)
			continue;
		// 如果没有视频数据源标志或处于浮动状态，继续寻找...
		if (bIsFloated || (flags & OBS_SOURCE_VIDEO) == 0)
			continue;
		vec2 vPos = { 1.0f, 1.0f };
		obs_sceneitem_get_pos(item, &vPos);
		// 如果数据源是在0点位置，继续寻找...
		if (vPos.x <= 0.0f && vPos.y <= 0.0f)
			continue;
		// 设定这个数据源的新位置...
		vPos.x -= nMoveLeftX;
		obs_sceneitem_set_pos(item, &vPos);
	}
}

// 为所有的互动学生端数据源创建第三方麦克风按钮...
void OBSBasic::doBuildAllStudentBtnMic()
{
	// 遍历所有的数据源对象，找到互动学生端对象，注意关联数据源...
	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem * listItem = ui->sources->item(i);
		obs_sceneitem_t * item = this->GetSceneItem(listItem);
		// 创建数据源对应的麦克风按钮 => 需要处理关联数据源...
		ui->preview->doBuildStudentBtnMic(item);
	}
}

// 调用预览窗口创建学生端麦克风按钮，并重排所有麦克风按钮...
void OBSBasic::doBuildStudentBtnMic(obs_sceneitem_t * lpSceneItem)
{
	ui->preview->doBuildStudentBtnMic(lpSceneItem);
	ui->preview->ResizeBtnMicAll();
}

// 判断是否显示左右箭头 => 第一次运行时需要保存0点位置数据源...
void OBSBasic::doCheckBtnPage(bool bIsFirst/* = false*/)
{
	// 获取显示系统的宽和高...
	obs_video_info ovi = { 0 };
	obs_get_video_info(&ovi);
	bool bIsShowLeft = false;
	bool bIsShowRight = false;
	// 遍历所有的数据源对象，判断是否需要显示左右翻页箭头按钮...
	for (int i = 0; i < ui->sources->count(); i++) {
		QListWidgetItem * listItem = ui->sources->item(i);
		obs_sceneitem_t * item = this->GetSceneItem(listItem);
		obs_source_t * source = obs_sceneitem_get_source(item);
		uint32_t flags = obs_source_get_output_flags(source);
		bool bIsFloated = obs_sceneitem_floated(item);
		// 如果数据源对象无效，继续寻找...
		if (item == NULL || source == NULL)
			continue;
		// 如果没有视频数据源标志或处于浮动状态，继续寻找...
		if (bIsFloated || (flags & OBS_SOURCE_VIDEO) == 0)
			continue;
		// 获取当前数据源的显示位置...
		vec2 vPos = { 1.0f, 1.0f };
		obs_sceneitem_get_pos(item, &vPos);
		// 如果是第一次，需要保存0点数据源 => 不要调用doCheckPPTSource()...
		// 因为，PPT数据源还没准备完毕，需要ss_update之后才准备完毕...
		// ss会发起source_updated信号，执行UpdatedSourceItem()...
		if (bIsFirst && vPos.x == 0.0f && vPos.y == 0.0f) {
			m_lpZeroSceneItem = item;
		}
		// < 0 => 显示左侧按钮..
		if (vPos.x < 0.0f) {
			bIsShowLeft = true;
			continue;
		}
		// > ovi.base_width => 显示右侧按钮...
		if (vPos.x > ovi.base_width) {
			bIsShowRight = true;
			continue;
		}
	}
	// 根据最终计算的结果显示左右翻页按钮...
	ui->preview->DispBtnLeft(bIsShowLeft);
	ui->preview->DispBtnRight(bIsShowRight);
}

// 注意：事先假定doCheckBtnPage先执行，已保存0点数据源...
// source完全加载更新完毕通知，这里只处理0点数据源通知...
void OBSBasic::UpdatedSourceItem(OBSSource source)
{
	// 如果不是0点数据源，或0点数据源无效，直接返回...
	obs_source_t * lpSource = obs_sceneitem_get_source(m_lpZeroSceneItem);
	if (lpSource == NULL || lpSource != source)
		return;
	// 更新0点幻灯片数据源信息...
	ASSERT(lpSource == source);
	this->doCheckPPTSource(lpSource);
}

void OBSBasic::doCheckPPTSource(obs_source_t * source)
{
	const char * lpSrcID = obs_source_get_id(source);
	bool bIsSlideShow = ((astrcmpi(lpSrcID, "slideshow") == 0) ? true : false);
	int nCurItem = 0; int nFileNum = 0;
	const char * lpName = NULL;
	if (bIsSlideShow) {
		obs_data_t * settings = obs_source_get_settings(source);
		lpName = obs_data_get_string(settings, "item_name");
		nCurItem = obs_data_get_int(settings, "cur_item");
		nFileNum = obs_data_get_int(settings, "file_num");
		obs_data_release(settings);
	}
	// 是否显示上一页或下一页按钮标志...
	ui->preview->DispBtnPrev(bIsSlideShow);
	ui->preview->DispBtnNext(bIsSlideShow);
	// 页码栏需要当前播放编号和总幻灯片个数...
	ui->preview->DispBtnFoot(bIsSlideShow, nCurItem, nFileNum, lpName);
}

// 将当前场景资源设置为第一个显示资源，重新计算显示坐标...
void OBSBasic::doSceneItemToFirst(obs_sceneitem_t * select_item)
{
	// 输入资源无效，直接返回...
	if (select_item == NULL)
		return;
	// 获取显示系统的宽和高...
	obs_video_info ovi = { 0 };
	obs_get_video_info(&ovi);
	// 计算第一个资源的宽和高...
	ovi.base_height -= DEF_ROW_SPACE;
	uint32_t first_width = ovi.base_width;
	uint32_t first_height = ovi.base_height - ovi.base_height / DEF_ROW_SIZE;
	// 设置默认的场景资源参数...
	obs_transform_info itemInfo = { 0 };
	vec2_set(&itemInfo.scale, 1.0f, 1.0f);
	itemInfo.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	itemInfo.bounds_type = OBS_BOUNDS_SCALE_INNER;
	itemInfo.bounds_alignment = OBS_ALIGN_CENTER;
	vec2_set(&itemInfo.pos, 0.0f, 0.0f);
	vec2_set(&itemInfo.bounds, float(first_width), float(first_height));
	// 更新场景资源的显示位置信息...
	obs_sceneitem_set_info(select_item, &itemInfo);
	// 设置场景资源的裁剪区域 => 不要重置裁剪区...
	//obs_sceneitem_crop crop = { 0 };
	//obs_sceneitem_set_crop(select_item, &crop);
	// 轨道1|轨道2 => 强制第一个窗口资源的音频输出，只保留全局音频资源和第一个窗口的音频资源输出...
	setAudioMixer(select_item, 0, true);
	setAudioMixer(select_item, 1, true);
	// 注意：doSendCameraPusherID 会在外层调用...
	// 0点位置如果是幻灯片数据源，需要显示上下页按钮...
	obs_source_t * lpSource = obs_sceneitem_get_source(select_item);
	this->doCheckPPTSource(lpSource);
}

// 注意：这里是进行第三方监听学生端的变化通知...
// 交互学生端摄像头编号发生变化，向服务器发送更新命令...
void OBSBasic::doSendCameraPusherID(int nDBCameraID)
{
	if (m_nDBCameraPusherID != nDBCameraID) {
		App()->doSendCameraPusherIDCmd(nDBCameraID);
		m_nDBCameraPusherID = nDBCameraID;
	}
}

// 第三方学生监听状态由讲师自己手动控制，不再通过焦点状态切换控制...
/*void OBSBasic::doSendCameraPusherID(obs_sceneitem_t * select_item)
{
	int nDBCameraID = 0;
	obs_source_t * lpSource = obs_sceneitem_get_source(select_item);
	const char * lpSrcID = obs_source_get_id(lpSource);
	// 获取当前数据源的摄像头编号配置信息，注意释放配置...
	if (astrcmpi(lpSrcID, App()->InteractRtpSource()) == 0) {
		obs_data_t * lpSettings = obs_source_get_settings(lpSource);
		nDBCameraID = obs_data_get_int(lpSettings, "camera_id");
		obs_data_release(lpSettings);
	}
	// 交互学生端摄像头编号发生变化，向服务器发送更新命令...
	if (m_nDBCameraPusherID != nDBCameraID) {
		App()->doSendCameraPusherIDCmd(nDBCameraID);
		m_nDBCameraPusherID = nDBCameraID;
	}
}*/

void OBSBasic::doHideDShowAudioMixer(obs_sceneitem_t * scene_item)
{
	obs_source_t * source = obs_sceneitem_get_source(scene_item);
	const char * lpID = obs_source_get_id(source);
	if (lpID != NULL && astrcmpi(lpID, App()->DShowInputSource()) == 0) {
		SetSourceMixerHidden(source, true);
		DeactivateAudioSource(source);
	}
}

// 思路错误 => 轨道3 => 输出给本地播放，数据源大于OBS_MONITORING_TYPE_NONE时，才进行混音处理...
void OBSBasic::doLocalPlayAudioMixer(obs_source_t * source)
{
	bool enabled = ((obs_source_get_monitoring_type(source) > OBS_MONITORING_TYPE_NONE) ? true : false);
	uint32_t new_mixers = obs_source_get_audio_mixers(source);
	// 针对轨道3(索引编号是2)的添加混音或取消混音处理...
	if (enabled) new_mixers |= (1 << 2);
	else         new_mixers &= ~(1 << 2);
	// 执行具体的新数据源的针对轨道3的混音处理接口...
	obs_source_set_audio_mixers(source, new_mixers);
}

void OBSBasic::OpenStudioProgramProjector()
{
	int monitor = sender()->property("monitor").toInt();
	OpenProjector(nullptr, monitor, nullptr, ProjectorType::StudioProgram);
}

void OBSBasic::OpenPreviewProjector()
{
	int monitor = sender()->property("monitor").toInt();
	OpenProjector(nullptr, monitor, nullptr, ProjectorType::Preview);
}

void OBSBasic::OpenSourceProjector()
{
	int monitor = sender()->property("monitor").toInt();
	OBSSceneItem item = this->GetCurrentSceneItem();
	if (!item) return;
	OpenProjector(obs_sceneitem_get_source(item), monitor, nullptr, ProjectorType::Source);
}

void OBSBasic::OpenMultiviewProjector()
{
	int monitor = sender()->property("monitor").toInt();
	OpenProjector(nullptr, monitor, nullptr, ProjectorType::Multiview);
}

void OBSBasic::OpenSceneProjector()
{
	int monitor = sender()->property("monitor").toInt();
	OBSScene scene = GetCurrentScene();
	if (!scene)
		return;

	OpenProjector(obs_scene_get_source(scene), monitor, nullptr,
			ProjectorType::Scene);
}

void OBSBasic::OpenStudioProgramWindow()
{
	OpenProjector(nullptr, -1, QTStr("StudioProgramWindow"),
			ProjectorType::StudioProgram);
}

void OBSBasic::OpenPreviewWindow()
{
	OpenProjector(nullptr, -1, QTStr("PreviewWindow"),
			ProjectorType::Preview);
}

void OBSBasic::OpenSourceWindow()
{
	OBSSceneItem item = GetCurrentSceneItem();
	if (!item)
		return;

	OBSSource source = obs_sceneitem_get_source(item);
	QString title = QString::fromUtf8(obs_source_get_name(source));

	OpenProjector(obs_sceneitem_get_source(item), -1, title,
			ProjectorType::Source);
}

void OBSBasic::OpenMultiviewWindow()
{
	OpenProjector(nullptr, -1, QTStr("MultiviewWindowed"),
			ProjectorType::Multiview);
}

void OBSBasic::OpenSceneWindow()
{
	OBSScene scene = GetCurrentScene();
	if (!scene)
		return;

	OBSSource source = obs_scene_get_source(scene);
	QString title = QString::fromUtf8(obs_source_get_name(source));

	OpenProjector(obs_scene_get_source(scene), -1, title,
			ProjectorType::Scene);
}

void OBSBasic::OpenSavedProjectors()
{
	for (SavedProjectorInfo *info : savedProjectorsArray) {
		OBSProjector *projector = nullptr;
		switch (info->type) {
		case ProjectorType::Source:
		case ProjectorType::Scene: {
			OBSSource source = obs_get_source_by_name(
					info->name.c_str());
			if (!source)
				continue;

			QString title = nullptr;
			if (info->monitor < 0)
				title = QString::fromUtf8(
						obs_source_get_name(source));

			projector = OpenProjector(source, info->monitor, title,
					info->type);

			obs_source_release(source);
			break;
		}
		case ProjectorType::Preview: {
			projector = OpenProjector(nullptr, info->monitor,
					QTStr("PreviewWindow"),
					ProjectorType::Preview);
			break;
		}
		case ProjectorType::StudioProgram: {
			projector = OpenProjector(nullptr, info->monitor,
					QTStr("StudioProgramWindow"),
					ProjectorType::StudioProgram);
			break;
		}
		case ProjectorType::Multiview: {
			projector = OpenProjector(nullptr, info->monitor,
					QTStr("MultiviewWindowed"),
					ProjectorType::Multiview);
			break;
		}
		}

		if (!info->geometry.empty()) {
			QByteArray byteArray = QByteArray::fromBase64(
					QByteArray(info->geometry.c_str()));
			projector->restoreGeometry(byteArray);

			if (!WindowPositionValid(projector->normalGeometry())) {
				QRect rect = App()->desktop()->geometry();
				projector->setGeometry(QStyle::alignedRect(
						Qt::LeftToRight,
						Qt::AlignCenter, size(), rect));
			}
		}
	}
}

void OBSBasic::on_actionFullscreenInterface_triggered()
{
	if (!fullscreenInterface)
		showFullScreen();
	else
		showNormal();

	fullscreenInterface = !fullscreenInterface;
}

void OBSBasic::UpdateTitleBar()
{
	stringstream name;

	const char *profile = config_get_string(App()->GlobalConfig(),
			"Basic", "Profile");
	const char *sceneCollection = config_get_string(App()->GlobalConfig(),
			"Basic", "SceneCollection");

	name << "OBS ";
	if (previewProgramMode)
		name << "Studio ";

	name << App()->GetVersionString();
	if (App()->IsPortableMode())
		name << " - Portable Mode";

	name << " - " << Str("TitleBar.Profile") << ": " << profile;
	name << " - " << Str("TitleBar.Scenes") << ": " << sceneCollection;
	
	//setWindowTitle(QT_UTF8(name.str().c_str()));

	// 对窗口标题进行修改 => 使用字典模式...
	string & strRoomID = App()->GetRoomIDStr();
	QString strTitle = QString("%1%2").arg(QTStr("Main.Window.TitleContent")).arg(QString::fromUtf8(strRoomID.c_str()));
	this->setWindowTitle(strTitle);
}

int OBSBasic::GetProfilePath(char *path, size_t size, const char *file) const
{
	char profiles_path[512];
	const char *profile = config_get_string(App()->GlobalConfig(),
			"Basic", "ProfileDir");
	int ret;

	if (!profile)
		return -1;
	if (!path)
		return -1;
	if (!file)
		file = "";

	ret = GetConfigPath(profiles_path, 512, "obs-teacher/basic/profiles");
	if (ret <= 0)
		return ret;

	if (!*file)
		return snprintf(path, size, "%s/%s", profiles_path, profile);

	return snprintf(path, size, "%s/%s/%s", profiles_path, profile, file);
}

void OBSBasic::on_resetUI_triggered()
{
	restoreState(startingDockLayout);

#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
	int cx = width();
	int cy = height();

	int cx22_5 = cx * 225 / 1000;
	int cx5 = cx * 5 / 100;

	cy = cy * 225 / 1000;

	int mixerSize = cx - (cx22_5 * 2 + cx5 * 2);

	QList<QDockWidget*> docks {
		ui->scenesDock,
		ui->sourcesDock,
		ui->mixerDock,
		ui->transitionsDock,
		ui->controlsDock
	};

	QList<int> sizes {
		cx22_5,
		cx22_5,
		mixerSize,
		cx5,
		cx5
	};

	ui->scenesDock->setVisible(true);
	ui->sourcesDock->setVisible(true);
	ui->mixerDock->setVisible(true);
	ui->transitionsDock->setVisible(true);
	ui->controlsDock->setVisible(true);

	resizeDocks(docks, {cy, cy, cy, cy, cy}, Qt::Vertical);
	resizeDocks(docks, sizes, Qt::Horizontal);
#endif
}

void OBSBasic::on_lockUI_toggled(bool lock)
{
	QDockWidget::DockWidgetFeatures features = lock
		? QDockWidget::NoDockWidgetFeatures
		: QDockWidget::AllDockWidgetFeatures;

	ui->scenesDock->setFeatures(features);
	ui->sourcesDock->setFeatures(features);
	ui->mixerDock->setFeatures(features);
	ui->transitionsDock->setFeatures(features);
	ui->controlsDock->setFeatures(features);
}

void OBSBasic::on_toggleListboxToolbars_toggled(bool visible)
{
	ui->sourcesToolbar->setVisible(visible);
	ui->scenesToolbar->setVisible(visible);

	config_set_bool(App()->GlobalConfig(), "BasicWindow",
			"ShowListboxToolbars", visible);
}

void OBSBasic::on_toggleStatusBar_toggled(bool visible)
{
	ui->statusbar->setVisible(visible);

	config_set_bool(App()->GlobalConfig(), "BasicWindow",
			"ShowStatusBar", visible);
}

void OBSBasic::on_actionLockPreview_triggered()
{
	ui->preview->ToggleLocked();
	ui->actionLockPreview->setChecked(ui->preview->Locked());
}

void OBSBasic::on_scalingMenu_aboutToShow()
{
	obs_video_info ovi;
	obs_get_video_info(&ovi);

	QAction *action = ui->actionScaleCanvas;
	QString text = QTStr("Basic.MainMenu.Edit.Scale.Canvas");
	text = text.arg(QString::number(ovi.base_width),
			QString::number(ovi.base_height));
	action->setText(text);

	action = ui->actionScaleOutput;
	text = QTStr("Basic.MainMenu.Edit.Scale.Output");
	text = text.arg(QString::number(ovi.output_width),
			QString::number(ovi.output_height));
	action->setText(text);
	action->setVisible(!(ovi.output_width == ovi.base_width &&
			ovi.output_height == ovi.base_height));

	UpdatePreviewScalingMenu();
}

void OBSBasic::on_actionScaleWindow_triggered()
{
	ui->preview->SetFixedScaling(false);
	ui->preview->ResetScrollingOffset();
	emit ui->preview->DisplayResized();
}

void OBSBasic::on_actionScaleCanvas_triggered()
{
	ui->preview->SetFixedScaling(true);
	ui->preview->SetScalingLevel(0);
	emit ui->preview->DisplayResized();
}

void OBSBasic::on_actionScaleOutput_triggered()
{
	obs_video_info ovi;
	obs_get_video_info(&ovi);

	ui->preview->SetFixedScaling(true);
	float scalingAmount = float(ovi.output_width) / float(ovi.base_width);
	// log base ZOOM_SENSITIVITY of x = log(x) / log(ZOOM_SENSITIVITY)
	int32_t approxScalingLevel = int32_t(round(log(scalingAmount) / log(ZOOM_SENSITIVITY)));
	ui->preview->SetScalingLevel(approxScalingLevel);
	ui->preview->SetScalingAmount(scalingAmount);
	emit ui->preview->DisplayResized();
}

void OBSBasic::SetShowing(bool showing)
{
	if (!showing && isVisible()) {
		config_set_string(App()->GlobalConfig(),
			"BasicWindow", "geometry",
			saveGeometry().toBase64().constData());

		/* hide all visible child dialogs */
		visDlgPositions.clear();
		if (!visDialogs.isEmpty()) {
			for (QDialog *dlg : visDialogs) {
				visDlgPositions.append(dlg->pos());
				dlg->hide();
			}
		}

		if (showHide)
			showHide->setText(QTStr("Basic.SystemTray.Show"));
		QTimer::singleShot(250, this, SLOT(hide()));

		if (previewEnabled)
			EnablePreviewDisplay(false);

		setVisible(false);

	} else if (showing && !isVisible()) {
		if (showHide)
			showHide->setText(QTStr("Basic.SystemTray.Hide"));
		QTimer::singleShot(250, this, SLOT(show()));

		if (previewEnabled)
			EnablePreviewDisplay(true);

		setVisible(true);

		/* show all child dialogs that was visible earlier */
		if (!visDialogs.isEmpty()) {
			for (int i = 0; i < visDialogs.size(); ++i) {
				QDialog *dlg = visDialogs[i];
				dlg->move(visDlgPositions[i]);
				dlg->show();
			}
		}

		/* Unminimize window if it was hidden to tray instead of task
		 * bar. */
		if (sysTrayMinimizeToTray()) {
			Qt::WindowStates state;
			state  = windowState() & ~Qt::WindowMinimized;
			state |= Qt::WindowActive;
			setWindowState(state);
		}
	}
}

void OBSBasic::ToggleShowHide()
{
	bool showing = isVisible();
	if (showing) {
		/* check for modal dialogs */
		EnumDialogs();
		if (!modalDialogs.isEmpty() || !visMsgBoxes.isEmpty())
			return;
	}
	SetShowing(!showing);
}

void OBSBasic::SystemTrayInit()
{
	ProfileScope("OBSBasic::SystemTrayInit");

	trayIcon = new QSystemTrayIcon(QIcon(":/res/images/obs.png"), this);
	trayIcon->setToolTip("Teacher");

	showHide = new QAction(QTStr("Basic.SystemTray.Show"), trayIcon);
	sysTrayStream = new QAction(QTStr("Basic.Main.StartStreaming"), trayIcon);
	sysTrayRecord = new QAction(QTStr("Basic.Main.StartRecording"), trayIcon);
	sysTrayReplayBuffer = new QAction(QTStr("Basic.Main.StartReplayBuffer"), trayIcon);
	exit = new QAction(QTStr("Exit"), trayIcon);

	if (outputHandler && !outputHandler->replayBuffer)
		sysTrayReplayBuffer->setEnabled(false);

	connect(trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
			this, SLOT(IconActivated(QSystemTrayIcon::ActivationReason)));
	connect(showHide, SIGNAL(triggered()), this, SLOT(ToggleShowHide()));
	connect(sysTrayStream, SIGNAL(triggered()), this, SLOT(on_streamButton_clicked()));
	connect(sysTrayRecord, SIGNAL(triggered()), this, SLOT(on_recordButton_clicked()));
	connect(sysTrayReplayBuffer.data(), &QAction::triggered, this, &OBSBasic::ReplayBufferClicked);
	connect(exit, SIGNAL(triggered()), this, SLOT(close()));

	trayMenu = new QMenu;
}

void OBSBasic::IconActivated(QSystemTrayIcon::ActivationReason reason)
{
	if (reason == QSystemTrayIcon::Trigger) {
		ToggleShowHide();
	} else if (reason == QSystemTrayIcon::Context) {
		QMenu *previewProjector = new QMenu(QTStr("PreviewProjector"));
		AddProjectorMenuMonitors(previewProjector, this,
				SLOT(OpenPreviewProjector()));
		QMenu *studioProgramProjector = new QMenu(
				QTStr("StudioProgramProjector"));
		AddProjectorMenuMonitors(studioProgramProjector, this,
				SLOT(OpenStudioProgramProjector()));

		trayMenu->clear();
		trayMenu->addAction(showHide);
		trayMenu->addMenu(previewProjector);
		trayMenu->addMenu(studioProgramProjector);
		trayMenu->addAction(sysTrayStream);
		trayMenu->addAction(sysTrayRecord);
		trayMenu->addAction(sysTrayReplayBuffer);
		trayMenu->addAction(exit);
		trayMenu->popup(QCursor::pos());
	}
}

void OBSBasic::SysTrayNotify(const QString &text,
		QSystemTrayIcon::MessageIcon n)
{
	if (QSystemTrayIcon::supportsMessages()) {
		QSystemTrayIcon::MessageIcon icon =
				QSystemTrayIcon::MessageIcon(n);
		trayIcon->showMessage("Teacher", text, icon, 10000);
	}
}

void OBSBasic::SystemTray(bool firstStarted)
{
	ProfileScope("OBSBasic::SystemTray");

	if (!QSystemTrayIcon::isSystemTrayAvailable())
		return;

	bool sysTrayWhenStarted = config_get_bool(GetGlobalConfig(),
			"BasicWindow", "SysTrayWhenStarted");
	bool sysTrayEnabled = config_get_bool(GetGlobalConfig(),
			"BasicWindow", "SysTrayEnabled");

	if (firstStarted)
		SystemTrayInit();

	if (!sysTrayWhenStarted && !sysTrayEnabled) {
		trayIcon->hide();
	} else if ((sysTrayWhenStarted && sysTrayEnabled)
			|| opt_minimize_tray) {
		trayIcon->show();
		if (firstStarted) {
			QTimer::singleShot(50, this, SLOT(hide()));
			EnablePreviewDisplay(false);
			setVisible(false);
			opt_minimize_tray = false;
		}
	} else if (sysTrayEnabled) {
		trayIcon->show();
	} else if (!sysTrayEnabled) {
		trayIcon->hide();
	} else if (!sysTrayWhenStarted && sysTrayEnabled) {
		trayIcon->hide();
	}

	if (isVisible())
		showHide->setText(QTStr("Basic.SystemTray.Hide"));
	else
		showHide->setText(QTStr("Basic.SystemTray.Show"));
}

bool OBSBasic::sysTrayMinimizeToTray()
{
	return config_get_bool(GetGlobalConfig(),
			"BasicWindow", "SysTrayMinimizeToTray");
}

void OBSBasic::on_actionCopySource_triggered()
{
	on_actionCopyTransform_triggered();

	OBSSceneItem item = GetCurrentSceneItem();

	if (!item)
		return;

	OBSSource source = obs_sceneitem_get_source(item);

	copyString = obs_source_get_name(source);
	copyVisible = obs_sceneitem_visible(item);

	//ui->actionPasteRef->setEnabled(true);

	uint32_t output_flags = obs_source_get_output_flags(source);
	if ((output_flags & OBS_SOURCE_DO_NOT_DUPLICATE) == 0)
		ui->actionPasteDup->setEnabled(true);
	else
		ui->actionPasteDup->setEnabled(false);
}

void OBSBasic::on_actionPasteRef_triggered()
{
	OBSBasicSourceSelect::SourcePaste(copyString, copyVisible, false);
	on_actionPasteTransform_triggered();
}

void OBSBasic::on_actionPasteDup_triggered()
{
	OBSBasicSourceSelect::SourcePaste(copyString, copyVisible, true);
	on_actionPasteTransform_triggered();
}

void OBSBasic::on_actionCopyFilters_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();

	if (!item)
		return;

	OBSSource source = obs_sceneitem_get_source(item);

	copyFiltersString = obs_source_get_name(source);

//	ui->actionPasteFilters->setEnabled(true);
}

void OBSBasic::on_actionPasteFilters_triggered()
{
	OBSSource source = obs_get_source_by_name(copyFiltersString);
	obs_source_release(source);

	OBSSceneItem sceneItem = GetCurrentSceneItem();
	OBSSource dstSource = obs_sceneitem_get_source(sceneItem);

	if (source == dstSource)
		return;

	obs_source_copy_filters(dstSource, source);
}

void OBSBasic::on_autoConfigure_triggered()
{
/*	AutoConfig test(this);
	test.setModal(true);
	test.show();
	test.exec();*/
}

void OBSBasic::on_stats_triggered()
{
	if (!stats.isNull()) {
		stats->show();
		stats->raise();
		return;
	}

	OBSBasicStats *statsDlg;
	statsDlg = new OBSBasicStats(nullptr);
	statsDlg->show();
	stats = statsDlg;
}
