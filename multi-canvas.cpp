
#include "multi-canvas.hpp"

#include <list>

#include "version.h"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QMainWindow>
#include <QPushButton>
#include <QMouseEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QMessageBox>

#include "config-dialog.hpp"
#include "display-helpers.hpp"
#include "media-io/video-frame.h"
#include "util/config-file.h"
#include "util/dstr.h"
#include "util/platform.h"
#include "util/util.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Aitum");
OBS_MODULE_USE_DEFAULT_LOCALE("transition-table", "en-US")

#define HANDLE_RADIUS 4.0f
#define HANDLE_SEL_RADIUS (HANDLE_RADIUS * 1.5f)
#define HELPER_ROT_BREAKPONT 45.0f

#define SPACER_LABEL_MARGIN 6.0f

inline std::list<CanvasDock *> canvas_docks;

void clear_canvas_docks()
{
	for (const auto &it : canvas_docks) {
		it->close();
		it->deleteLater();
	}
	canvas_docks.clear();
}

void frontend_save_load(obs_data_t *save_data, bool saving, void *private_data)
{
	UNUSED_PARAMETER(save_data);
	UNUSED_PARAMETER(private_data);
	if (saving)
		return;

	clear_canvas_docks();

	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	obs_source_t *found = nullptr;
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *src = scenes.sources.array[i];
		obs_data_t *settings = obs_source_get_settings(src);
		if (obs_data_get_bool(settings, "custom_size")) {
			found = src;
		}
		obs_data_release(settings);
	}
	if (found == nullptr) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_bool(settings, "custom_size", true);
		obs_data_set_int(settings, "cx", 1080);
		obs_data_set_int(settings, "cy", 1920);
		obs_data_array_t *items = obs_data_array_create();
		obs_data_set_array(settings, "items", items);
		obs_data_array_release(items);
		found = obs_source_create("scene", "Multi Canvas", settings,
					  nullptr);
		obs_source_load(found);
		obs_source_release(found);
		obs_data_release(settings);
	}
	obs_frontend_source_list_free(&scenes);

	const auto main_window =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());
	const auto dock = new CanvasDock(obs_source_get_base_width(found),
					 obs_source_get_base_height(found),
					 main_window);
	auto *a = static_cast<QAction *>(obs_frontend_add_dock(dock));
	dock->setAction(a);
	dock->setSource(obs_source_get_weak_source(found));
	canvas_docks.push_back(dock);
}

void frontend_event(obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);
	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP ||
	    event == OBS_FRONTEND_EVENT_EXIT) {
		clear_canvas_docks();
	}
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Multi Canvas] loaded version %s", PROJECT_VERSION);
	obs_frontend_add_save_callback(frontend_save_load, nullptr);
	obs_frontend_add_event_callback(frontend_event, nullptr);
	return true;
}

void obs_module_unload(void) {}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("MultiCanvas");
}

CanvasDock::CanvasDock(uint32_t width, uint32_t height, QWidget *parent)
	: QDockWidget(parent),
	  action(nullptr),
	  mainLayout(new QVBoxLayout(this)),
	  preview(new OBSQTDisplay(this)),
	  eventFilter(BuildEventFilter())
{
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	setFeatures(DockWidgetClosable | DockWidgetMovable |
		    DockWidgetFloatable);
	setWindowTitle("Multi Canvas");
	setObjectName("Multi Canvas");
	setFloating(true);

	auto *dockWidgetContents = new QWidget;
	dockWidgetContents->setObjectName(QStringLiteral("contextContainer"));
	dockWidgetContents->setLayout(mainLayout);

	setWidget(dockWidgetContents);

	preview->setObjectName(QStringLiteral("preview"));
	preview->setMinimumSize(QSize(24, 24));
	QSizePolicy sizePolicy1(QSizePolicy::Expanding, QSizePolicy::Expanding);
	sizePolicy1.setHorizontalStretch(0);
	sizePolicy1.setVerticalStretch(0);
	sizePolicy1.setHeightForWidth(
		preview->sizePolicy().hasHeightForWidth());
	preview->setSizePolicy(sizePolicy1);

	preview->setMouseTracking(true);
	preview->setFocusPolicy(Qt::StrongFocus);
	preview->installEventFilter(eventFilter.get());

	auto addDrawCallback = [this]() {
		obs_display_add_draw_callback(preview->GetDisplay(),
					      DrawPreview, this);
	};
	preview->show();
	connect(preview, &OBSQTDisplay::DisplayCreated, addDrawCallback);
	mainLayout->addWidget(preview);

	auto buttonRow = new QHBoxLayout(this);

	virtualCamButton = new QPushButton;
	virtualCamButton->setObjectName(QStringLiteral("canvasVirtualCam"));
	virtualCamButton->setText(obs_module_text("VirtualCam"));
	virtualCamButton->setCheckable(true);
	virtualCamButton->setChecked(false);
	connect(virtualCamButton, SIGNAL(clicked()), this,
		SLOT(VirtualCamButtonClicked()));
	buttonRow->addWidget(virtualCamButton);

	auto replayButton = new QPushButton;
	replayButton->setObjectName(QStringLiteral("canvasReplay"));
	replayButton->setText(obs_module_text("Replay"));
	connect(replayButton, SIGNAL(clicked()), this,
		SLOT(RecordButtonClicked()));
	buttonRow->addWidget(replayButton);

	recordButton = new QPushButton;
	recordButton->setObjectName(QStringLiteral("canvasRecord"));
	recordButton->setText(obs_module_text("Record"));
	recordButton->setCheckable(true);
	recordButton->setChecked(false);
	connect(recordButton, SIGNAL(clicked()), this,
		SLOT(RecordButtonClicked()));
	buttonRow->addWidget(recordButton);

	streamButton = new QPushButton;
	streamButton->setObjectName(QStringLiteral("canvasStream"));
	streamButton->setText(obs_module_text("Stream"));
	streamButton->setCheckable(true);
	streamButton->setChecked(false);
	connect(streamButton, SIGNAL(clicked()), this,
		SLOT(StreamButtonClicked()));
	buttonRow->addWidget(streamButton);

	auto *configButton = new QPushButton(this);
	configButton->setProperty("themeID", "configIconSmall");
	configButton->setFlat(true);
	configButton->setMaximumWidth(30);
	configButton->setAutoDefault(false);
	connect(configButton, SIGNAL(clicked()), this,
		SLOT(ConfigButtonClicked()));
	buttonRow->addWidget(configButton);

	mainLayout->addLayout(buttonRow);

	obs_enter_graphics();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(1.0f, 1.0f);
	box = gs_render_save();

	obs_leave_graphics();
}

CanvasDock::~CanvasDock()
{
	obs_display_remove_draw_callback(preview->GetDisplay(), DrawPreview,
					 this);
	delete action;

	if (recordOutput) {
		obs_output_stop(recordOutput);
		obs_output_release(recordOutput);
	}
	if (virtualCamOutput) {
		obs_output_stop(virtualCamOutput);
		obs_output_release(virtualCamOutput);
	}

	obs_enter_graphics();

	if (overflow)
		gs_texture_destroy(overflow);
	if (rectFill)
		gs_vertexbuffer_destroy(rectFill);
	if (circleFill)
		gs_vertexbuffer_destroy(circleFill);

	gs_vertexbuffer_destroy(box);
	obs_leave_graphics();
}

void CanvasDock::setAction(QAction *a)
{
	action = a;
}

void CanvasDock::setSource(obs_weak_source_t *source)
{
	this->source = source;
	auto s = obs_weak_source_get_source(source);
	this->scene = obs_scene_from_source(s);
	obs_source_release(s);
}

static bool SceneItemHasVideo(obs_sceneitem_t *item)
{
	const obs_source_t *source = obs_sceneitem_get_source(item);
	const uint32_t flags = obs_source_get_output_flags(source);
	return (flags & OBS_SOURCE_VIDEO) != 0;
}

void CanvasDock::DrawOverflow(float scale)
{
	if (locked)
		return;

	bool hidden = config_get_bool(obs_frontend_get_global_config(),
				      "BasicWindow", "OverflowHidden");

	if (hidden)
		return;

	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "DrawOverflow");

	if (!overflow) {
		overflow = gs_texture_create_from_file(
			obs_module_file("images/overflow.png"));
	}

	if (scene) {
		gs_matrix_push();
		gs_matrix_scale3f(scale, scale, 1.0f);
		obs_scene_enum_items(scene, DrawSelectedOverflow, this);
		gs_matrix_pop();
	}

	gs_load_vertexbuffer(nullptr);

	GS_DEBUG_MARKER_END();
}

static bool CloseFloat(float a, float b, float epsilon = 0.01f)
{
	return std::abs(a - b) <= epsilon;
}

bool CanvasDock::DrawSelectedOverflow(obs_scene_t *scene, obs_sceneitem_t *item,
				      void *param)
{
	if (obs_sceneitem_locked(item))
		return true;

	if (!SceneItemHasVideo(item))
		return true;

	bool select = config_get_bool(obs_frontend_get_global_config(),
				      "BasicWindow", "OverflowSelectionHidden");

	if (!select && !obs_sceneitem_visible(item))
		return true;

	if (obs_sceneitem_is_group(item)) {
		matrix4 mat;
		obs_sceneitem_get_draw_transform(item, &mat);

		gs_matrix_push();
		gs_matrix_mul(&mat);
		obs_sceneitem_group_enum_items(item, DrawSelectedOverflow,
					       param);
		gs_matrix_pop();
	}

	bool always = config_get_bool(obs_frontend_get_global_config(),
				      "BasicWindow", "OverflowAlwaysVisible");

	if (!always && !obs_sceneitem_selected(item))
		return true;

	CanvasDock *prev = reinterpret_cast<CanvasDock *>(param);

	matrix4 boxTransform;
	matrix4 invBoxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);
	matrix4_inv(&invBoxTransform, &boxTransform);

	vec3 bounds[] = {
		{{{0.f, 0.f, 0.f}}},
		{{{1.f, 0.f, 0.f}}},
		{{{0.f, 1.f, 0.f}}},
		{{{1.f, 1.f, 0.f}}},
	};

	bool visible = std::all_of(
		std::begin(bounds), std::end(bounds), [&](const vec3 &b) {
			vec3 pos;
			vec3_transform(&pos, &b, &boxTransform);
			vec3_transform(&pos, &pos, &invBoxTransform);
			return CloseFloat(pos.x, b.x) && CloseFloat(pos.y, b.y);
		});

	if (!visible)
		return true;

	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "DrawSelectedOverflow");

	obs_transform_info info;
	obs_sceneitem_get_info(item, &info);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_REPEAT);
	gs_eparam_t *image = gs_effect_get_param_by_name(solid, "image");
	gs_eparam_t *scale = gs_effect_get_param_by_name(solid, "scale");

	vec2 s;
	vec2_set(&s, boxTransform.x.x / 96, boxTransform.y.y / 96);

	gs_effect_set_vec2(scale, &s);
	gs_effect_set_texture(image, prev->overflow);

	gs_matrix_push();
	gs_matrix_mul(&boxTransform);

	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	while (gs_effect_loop(solid, "Draw")) {
		gs_draw_sprite(prev->overflow, 0, 1, 1);
	}

	gs_matrix_pop();

	GS_DEBUG_MARKER_END();

	UNUSED_PARAMETER(scene);
	return true;
}

void CanvasDock::DrawBackdrop(float cx, float cy)
{
	if (!box)
		return;

	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "DrawBackdrop");

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

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

	GS_DEBUG_MARKER_END();
}

void CanvasDock::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	CanvasDock *window = static_cast<CanvasDock *>(data);

	if (!window->source)
		return;
	auto source = obs_weak_source_get_source(window->source);
	if (!source)
		return;
	uint32_t sourceCX = obs_source_get_width(source);
	if (sourceCX <= 0)
		sourceCX = 1;
	uint32_t sourceCY = obs_source_get_height(source);
	if (sourceCY <= 0)
		sourceCY = 1;

	int x, y;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, cx, cy, x, y, scale);
	auto newCX = scale * float(sourceCX);
	auto newCY = scale * float(sourceCY);

	/*auto extraCx = (window->zoom - 1.0f) * newCX;
	auto extraCy = (window->zoom - 1.0f) * newCY;
	int newCx = newCX * window->zoom;
	int newCy = newCY * window->zoom;
	x -= extraCx * window->scrollX;
	y -= extraCy * window->scrollY;*/
	gs_viewport_push();
	gs_projection_push();

	gs_ortho(float(-x), newCX + float(x), float(-y), newCY + float(y),
		 -100.0f, 100.0f);
	gs_reset_viewport();

	window->DrawOverflow(scale);

	window->DrawBackdrop(newCX, newCY);

	const bool previous = gs_set_linear_srgb(true);

	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, newCX, newCY);
	obs_source_video_render(source);
	obs_source_release(source);

	gs_set_linear_srgb(previous);

	gs_ortho(float(-x), newCX + float(x), float(-y), newCY + float(y),
		 -100.0f, 100.0f);
	gs_reset_viewport();

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	if (window->scene && !window->locked) {
		gs_matrix_push();
		gs_matrix_scale3f(scale, scale, 1.0f);
		obs_scene_enum_items(window->scene, DrawSelectedItem, data);
		gs_matrix_pop();
	}

	if (window->selectionBox) {
		if (!window->rectFill) {
			gs_render_start(true);

			gs_vertex2f(0.0f, 0.0f);
			gs_vertex2f(1.0f, 0.0f);
			gs_vertex2f(0.0f, 1.0f);
			gs_vertex2f(1.0f, 1.0f);

			window->rectFill = gs_render_save();
		}

		window->DrawSelectionBox(window->startPos.x * scale,
					 window->startPos.y * scale,
					 window->mousePos.x * scale,
					 window->mousePos.y * scale,
					 window->rectFill);
	}

	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	if (window->drawSpacingHelpers)
		window->DrawSpacingHelpers(window->scene, x, y, newCX, newCY,
					   scale, float(sourceCX),
					   float(sourceCY));

	gs_projection_pop();
	gs_viewport_pop();
}

struct SceneFindData {
	const vec2 &pos;
	OBSSceneItem item;
	bool selectBelow;

	obs_sceneitem_t *group = nullptr;

	SceneFindData(const SceneFindData &) = delete;
	SceneFindData(SceneFindData &&) = delete;
	SceneFindData &operator=(const SceneFindData &) = delete;
	SceneFindData &operator=(SceneFindData &&) = delete;

	inline SceneFindData(const vec2 &pos_, bool selectBelow_)
		: pos(pos_), selectBelow(selectBelow_)
	{
	}
};

struct SceneFindBoxData {
	const vec2 &startPos;
	const vec2 &pos;
	std::vector<obs_sceneitem_t *> sceneItems;

	SceneFindBoxData(const SceneFindData &) = delete;
	SceneFindBoxData(SceneFindData &&) = delete;
	SceneFindBoxData &operator=(const SceneFindData &) = delete;
	SceneFindBoxData &operator=(SceneFindData &&) = delete;

	inline SceneFindBoxData(const vec2 &startPos_, const vec2 &pos_)
		: startPos(startPos_), pos(pos_)
	{
	}
};

bool CanvasDock::FindSelected(obs_scene_t *scene, obs_sceneitem_t *item,
			      void *param)
{
	SceneFindBoxData *data = reinterpret_cast<SceneFindBoxData *>(param);

	if (obs_sceneitem_selected(item))
		data->sceneItems.push_back(item);

	UNUSED_PARAMETER(scene);
	return true;
}

static vec2 GetItemSize(obs_sceneitem_t *item)
{
	obs_bounds_type boundsType = obs_sceneitem_get_bounds_type(item);
	vec2 size;

	if (boundsType != OBS_BOUNDS_NONE) {
		obs_sceneitem_get_bounds(item, &size);
	} else {
		obs_source_t *source = obs_sceneitem_get_source(item);
		obs_sceneitem_crop crop;
		vec2 scale;

		obs_sceneitem_get_scale(item, &scale);
		obs_sceneitem_get_crop(item, &crop);
		size.x = float(obs_source_get_width(source) - crop.left -
			       crop.right) *
			 scale.x;
		size.y = float(obs_source_get_height(source) - crop.top -
			       crop.bottom) *
			 scale.y;
	}

	return size;
}

static vec3 GetTransformedPos(float x, float y, const matrix4 &mat)
{
	vec3 result;
	vec3_set(&result, x, y, 0.0f);
	vec3_transform(&result, &result, &mat);
	return result;
}

static void DrawLine(float x1, float y1, float x2, float y2, float thickness,
		     vec2 scale)
{
	float ySide = (y1 == y2) ? (y1 < 0.5f ? 1.0f : -1.0f) : 0.0f;
	float xSide = (x1 == x2) ? (x1 < 0.5f ? 1.0f : -1.0f) : 0.0f;

	gs_render_start(true);

	gs_vertex2f(x1, y1);
	gs_vertex2f(x1 + (xSide * (thickness / scale.x)),
		    y1 + (ySide * (thickness / scale.y)));
	gs_vertex2f(x2 + (xSide * (thickness / scale.x)),
		    y2 + (ySide * (thickness / scale.y)));
	gs_vertex2f(x2, y2);
	gs_vertex2f(x1, y1);

	gs_vertbuffer_t *line = gs_render_save();

	gs_load_vertexbuffer(line);
	gs_draw(GS_TRISTRIP, 0, 0);
	gs_vertexbuffer_destroy(line);
}

void CanvasDock::DrawSpacingLine(vec3 &start, vec3 &end, vec3 &viewport,
				 float pixelRatio)
{
	matrix4 transform;
	matrix4_identity(&transform);
	transform.x.x = viewport.x;
	transform.y.y = viewport.y;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	QColor selColor = GetSelectionColor();
	vec4 color;
	vec4_set(&color, selColor.redF(), selColor.greenF(), selColor.blueF(),
		 1.0f);

	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &color);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	gs_matrix_push();
	gs_matrix_mul(&transform);

	vec2 scale;
	vec2_set(&scale, viewport.x, viewport.y);

	DrawLine(start.x, start.y, end.x, end.y,
		 pixelRatio * (HANDLE_RADIUS / 2), scale);

	gs_matrix_pop();

	gs_load_vertexbuffer(nullptr);

	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

void CanvasDock::SetLabelText(int sourceIndex, int px)
{

	if (px == spacerPx[sourceIndex])
		return;

	std::string text = std::to_string(px) + " px";

	obs_source_t *source = spacerLabel[sourceIndex];

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	obs_data_set_string(settings, "text", text.c_str());
	obs_source_update(source, settings);

	spacerPx[sourceIndex] = px;
}

static void DrawLabel(OBSSource source, vec3 &pos, vec3 &viewport)
{
	if (!source)
		return;

	vec3_mul(&pos, &pos, &viewport);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate(&pos);
	obs_source_video_render(source);
	gs_matrix_pop();
}

void CanvasDock::RenderSpacingHelper(int sourceIndex, vec3 &start, vec3 &end,
				     vec3 &viewport, float pixelRatio)
{
	bool horizontal = (sourceIndex == 2 || sourceIndex == 3);

	// If outside of preview, don't render
	if (!((horizontal && (end.x >= start.x)) ||
	      (!horizontal && (end.y >= start.y))))
		return;

	float length = vec3_dist(&start, &end);

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	float px;

	if (horizontal) {
		px = length * ovi.base_width;
	} else {
		px = length * ovi.base_height;
	}

	if (px <= 0.0f)
		return;

	obs_source_t *source = spacerLabel[sourceIndex];
	vec3 labelSize, labelPos;
	vec3_set(&labelSize, obs_source_get_width(source),
		 obs_source_get_height(source), 1.0f);

	vec3_div(&labelSize, &labelSize, &viewport);

	vec3 labelMargin;
	vec3_set(&labelMargin, SPACER_LABEL_MARGIN * pixelRatio,
		 SPACER_LABEL_MARGIN * pixelRatio, 1.0f);
	vec3_div(&labelMargin, &labelMargin, &viewport);

	vec3_set(&labelPos, end.x, end.y, end.z);
	if (horizontal) {
		labelPos.x -= (end.x - start.x) / 2;
		labelPos.x -= labelSize.x / 2;
		labelPos.y -= labelMargin.y + (labelSize.y / 2) +
			      (HANDLE_RADIUS / viewport.y);
	} else {
		labelPos.y -= (end.y - start.y) / 2;
		labelPos.y -= labelSize.y / 2;
		labelPos.x += labelMargin.x;
	}

	DrawSpacingLine(start, end, viewport, pixelRatio);
	SetLabelText(sourceIndex, (int)px);
	DrawLabel(source, labelPos, viewport);
}

static obs_source_t *CreateLabel(float pixelRatio)
{
	OBSDataAutoRelease settings = obs_data_create();
	OBSDataAutoRelease font = obs_data_create();

#if defined(_WIN32)
	obs_data_set_string(font, "face", "Arial");
#elif defined(__APPLE__)
	obs_data_set_string(font, "face", "Helvetica");
#else
	obs_data_set_string(font, "face", "Monospace");
#endif
	obs_data_set_int(font, "flags", 1); // Bold text
	obs_data_set_int(font, "size", 16 * pixelRatio);

	obs_data_set_obj(settings, "font", font);
	obs_data_set_bool(settings, "outline", true);

#ifdef _WIN32
	obs_data_set_int(settings, "outline_color", 0x000000);
	obs_data_set_int(settings, "outline_size", 3);
	const char *text_source_id = "text_gdiplus";
#else
	const char *text_source_id = "text_ft2_source";
#endif

	OBSSource txtSource =
		obs_source_create_private(text_source_id, nullptr, settings);

	return txtSource;
}

obs_scene_item *CanvasDock::GetSelectedItem()
{
	vec2 s;
	SceneFindBoxData data(s, s);

	obs_scene_enum_items(scene, FindSelected, &data);

	if (data.sceneItems.size() != 1)
		return nullptr;

	return data.sceneItems.at(0);
}

void CanvasDock::DrawSpacingHelpers(obs_scene_t *scene, float x, float y,
				    float cx, float cy, float scale,
				    float sourceX, float sourceY)
{
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);
	if (locked)
		return;

	OBSSceneItem item = GetSelectedItem();
	if (!item)
		return;

	if (obs_sceneitem_locked(item))
		return;

	vec2 itemSize = GetItemSize(item);
	if (itemSize.x == 0.0f || itemSize.y == 0.0f)
		return;

	obs_sceneitem_t *parentGroup = obs_sceneitem_get_group(scene, item);

	if (parentGroup && obs_sceneitem_locked(parentGroup))
		return;

	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	obs_transform_info oti;
	obs_sceneitem_get_info(item, &oti);

	vec3 size;
	vec3_set(&size, sourceX, sourceY, 1.0f);

	// Init box transform side locations
	vec3 left, right, top, bottom;

	vec3_set(&left, 0.0f, 0.5f, 1.0f);
	vec3_set(&right, 1.0f, 0.5f, 1.0f);
	vec3_set(&top, 0.5f, 0.0f, 1.0f);
	vec3_set(&bottom, 0.5f, 1.0f, 1.0f);

	// Decide which side to use with box transform, based on rotation
	// Seems hacky, probably a better way to do it
	float rot = oti.rot;

	if (parentGroup) {
		obs_transform_info groupOti;
		obs_sceneitem_get_info(parentGroup, &groupOti);

		//Correct the scene item rotation angle
		rot = oti.rot + groupOti.rot;

		// Correct the scene item box transform
		// Based on scale, rotation angle, position of parent's group
		matrix4_scale3f(&boxTransform, &boxTransform, groupOti.scale.x,
				groupOti.scale.y, 1.0f);
		matrix4_rotate_aa4f(&boxTransform, &boxTransform, 0.0f, 0.0f,
				    1.0f, RAD(groupOti.rot));
		matrix4_translate3f(&boxTransform, &boxTransform,
				    groupOti.pos.x, groupOti.pos.y, 0.0f);
	}

	if (rot >= HELPER_ROT_BREAKPONT) {
		for (float i = HELPER_ROT_BREAKPONT; i <= 360.0f; i += 90.0f) {
			if (rot < i)
				break;

			vec3 l = left;
			vec3 r = right;
			vec3 t = top;
			vec3 b = bottom;

			vec3_copy(&top, &l);
			vec3_copy(&right, &t);
			vec3_copy(&bottom, &r);
			vec3_copy(&left, &b);
		}
	} else if (rot <= -HELPER_ROT_BREAKPONT) {
		for (float i = -HELPER_ROT_BREAKPONT; i >= -360.0f;
		     i -= 90.0f) {
			if (rot > i)
				break;

			vec3 l = left;
			vec3 r = right;
			vec3 t = top;
			vec3 b = bottom;

			vec3_copy(&top, &r);
			vec3_copy(&right, &b);
			vec3_copy(&bottom, &l);
			vec3_copy(&left, &t);
		}
	}

	// Switch top/bottom or right/left if scale is negative
	if (oti.scale.x < 0.0f) {
		vec3 l = left;
		vec3 r = right;

		vec3_copy(&left, &r);
		vec3_copy(&right, &l);
	}

	if (oti.scale.y < 0.0f) {
		vec3 t = top;
		vec3 b = bottom;

		vec3_copy(&top, &b);
		vec3_copy(&bottom, &t);
	}

	// Get sides of box transform
	left = GetTransformedPos(left.x, left.y, boxTransform);
	right = GetTransformedPos(right.x, right.y, boxTransform);
	top = GetTransformedPos(top.x, top.y, boxTransform);
	bottom = GetTransformedPos(bottom.x, bottom.y, boxTransform);

	bottom.y = size.y - bottom.y;
	right.x = size.x - right.x;

	// Init viewport
	vec3 viewport;
	vec3_set(&viewport, cx, cy, 1.0f);

	vec3_div(&left, &left, &viewport);
	vec3_div(&right, &right, &viewport);
	vec3_div(&top, &top, &viewport);
	vec3_div(&bottom, &bottom, &viewport);

	vec3_mulf(&left, &left, scale);
	vec3_mulf(&right, &right, scale);
	vec3_mulf(&top, &top, scale);
	vec3_mulf(&bottom, &bottom, scale);

	// Draw spacer lines and labels
	vec3 start, end;

	float pixelRatio = 1.0f; //main->GetDevicePixelRatio();
	for (int i = 0; i < 4; i++) {
		if (!spacerLabel[i])
			spacerLabel[i] = CreateLabel(pixelRatio);
	}

	vec3_set(&start, top.x, 0.0f, 1.0f);
	vec3_set(&end, top.x, top.y, 1.0f);
	RenderSpacingHelper(0, start, end, viewport, pixelRatio);

	vec3_set(&start, bottom.x, 1.0f - bottom.y, 1.0f);
	vec3_set(&end, bottom.x, 1.0f, 1.0f);
	RenderSpacingHelper(1, start, end, viewport, pixelRatio);

	vec3_set(&start, 0.0f, left.y, 1.0f);
	vec3_set(&end, left.x, left.y, 1.0f);
	RenderSpacingHelper(2, start, end, viewport, pixelRatio);

	vec3_set(&start, 1.0f - right.x, right.y, 1.0f);
	vec3_set(&end, 1.0f, right.y, 1.0f);
	RenderSpacingHelper(3, start, end, viewport, pixelRatio);
}

static inline bool crop_enabled(const obs_sceneitem_crop *crop)
{
	return crop->left > 0 || crop->top > 0 || crop->right > 0 ||
	       crop->bottom > 0;
}

static void DrawSquareAtPos(float x, float y, float pixelRatio)
{
	struct vec3 pos;
	vec3_set(&pos, x, y, 0.0f);

	struct matrix4 matrix;
	gs_matrix_get(&matrix);
	vec3_transform(&pos, &pos, &matrix);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate(&pos);

	gs_matrix_translate3f(-HANDLE_RADIUS * pixelRatio,
			      -HANDLE_RADIUS * pixelRatio, 0.0f);
	gs_matrix_scale3f(HANDLE_RADIUS * pixelRatio * 2,
			  HANDLE_RADIUS * pixelRatio * 2, 1.0f);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
}

static void DrawRotationHandle(gs_vertbuffer_t *circle, float rot,
			       float pixelRatio)
{
	struct vec3 pos;
	vec3_set(&pos, 0.5f, 0.0f, 0.0f);

	struct matrix4 matrix;
	gs_matrix_get(&matrix);
	vec3_transform(&pos, &pos, &matrix);

	gs_render_start(true);

	gs_vertex2f(0.5f - 0.34f / HANDLE_RADIUS, 0.5f);
	gs_vertex2f(0.5f - 0.34f / HANDLE_RADIUS, -2.0f);
	gs_vertex2f(0.5f + 0.34f / HANDLE_RADIUS, -2.0f);
	gs_vertex2f(0.5f + 0.34f / HANDLE_RADIUS, 0.5f);
	gs_vertex2f(0.5f - 0.34f / HANDLE_RADIUS, 0.5f);

	gs_vertbuffer_t *line = gs_render_save();

	gs_load_vertexbuffer(line);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate(&pos);

	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD(rot));
	gs_matrix_translate3f(-HANDLE_RADIUS * 1.5 * pixelRatio,
			      -HANDLE_RADIUS * 1.5 * pixelRatio, 0.0f);
	gs_matrix_scale3f(HANDLE_RADIUS * 3 * pixelRatio,
			  HANDLE_RADIUS * 3 * pixelRatio, 1.0f);

	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_translate3f(0.0f, -HANDLE_RADIUS * 2 / 3, 0.0f);

	gs_load_vertexbuffer(circle);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
	gs_vertexbuffer_destroy(line);
}

static void DrawStripedLine(float x1, float y1, float x2, float y2,
			    float thickness, vec2 scale)
{
	float ySide = (y1 == y2) ? (y1 < 0.5f ? 1.0f : -1.0f) : 0.0f;
	float xSide = (x1 == x2) ? (x1 < 0.5f ? 1.0f : -1.0f) : 0.0f;

	float dist =
		sqrt(pow((x1 - x2) * scale.x, 2) + pow((y1 - y2) * scale.y, 2));
	float offX = (x2 - x1) / dist;
	float offY = (y2 - y1) / dist;

	for (int i = 0, l = ceil(dist / 15); i < l; i++) {
		gs_render_start(true);

		float xx1 = x1 + i * 15 * offX;
		float yy1 = y1 + i * 15 * offY;

		float dx;
		float dy;

		if (x1 < x2) {
			dx = std::min(xx1 + 7.5f * offX, x2);
		} else {
			dx = std::max(xx1 + 7.5f * offX, x2);
		}

		if (y1 < y2) {
			dy = std::min(yy1 + 7.5f * offY, y2);
		} else {
			dy = std::max(yy1 + 7.5f * offY, y2);
		}

		gs_vertex2f(xx1, yy1);
		gs_vertex2f(xx1 + (xSide * (thickness / scale.x)),
			    yy1 + (ySide * (thickness / scale.y)));
		gs_vertex2f(dx, dy);
		gs_vertex2f(dx + (xSide * (thickness / scale.x)),
			    dy + (ySide * (thickness / scale.y)));

		gs_vertbuffer_t *line = gs_render_save();

		gs_load_vertexbuffer(line);
		gs_draw(GS_TRISTRIP, 0, 0);
		gs_vertexbuffer_destroy(line);
	}
}

static void DrawRect(float thickness, vec2 scale)
{
	gs_render_start(true);

	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f + (thickness / scale.x), 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(0.0f + (thickness / scale.x), 1.0f);
	gs_vertex2f(0.0f, 1.0f - (thickness / scale.y));
	gs_vertex2f(1.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f - (thickness / scale.y));
	gs_vertex2f(1.0f - (thickness / scale.x), 1.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(1.0f - (thickness / scale.x), 0.0f);
	gs_vertex2f(1.0f, 0.0f + (thickness / scale.y));
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 0.0f + (thickness / scale.y));

	gs_vertbuffer_t *rect = gs_render_save();

	gs_load_vertexbuffer(rect);
	gs_draw(GS_TRISTRIP, 0, 0);
	gs_vertexbuffer_destroy(rect);
}

bool CanvasDock::DrawSelectedItem(obs_scene_t *scene, obs_sceneitem_t *item,
				  void *param)
{
	if (obs_sceneitem_locked(item))
		return true;

	if (!SceneItemHasVideo(item))
		return true;

	CanvasDock *window = static_cast<CanvasDock *>(param);

	if (obs_sceneitem_is_group(item)) {
		matrix4 mat;
		obs_transform_info groupInfo;
		obs_sceneitem_get_draw_transform(item, &mat);
		obs_sceneitem_get_info(item, &groupInfo);

		window->groupRot = groupInfo.rot;

		gs_matrix_push();
		gs_matrix_mul(&mat);
		obs_sceneitem_group_enum_items(item, DrawSelectedItem, param);
		gs_matrix_pop();

		window->groupRot = 0.0f;
	}

	//OBSBasic *main = OBSBasic::Get();

	float pixelRatio = 1.0; //main->GetDevicePixelRatio();

	bool hovered = false;
	{
		std::lock_guard<std::mutex> lock(window->selectMutex);
		for (size_t i = 0; i < window->hoveredPreviewItems.size();
		     i++) {
			if (window->hoveredPreviewItems[i] == item) {
				hovered = true;
				break;
			}
		}
	}

	bool selected = obs_sceneitem_selected(item);

	if (!selected && !hovered)
		return true;

	matrix4 boxTransform;
	matrix4 invBoxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);
	matrix4_inv(&invBoxTransform, &boxTransform);

	vec3 bounds[] = {
		{{{0.f, 0.f, 0.f}}},
		{{{1.f, 0.f, 0.f}}},
		{{{0.f, 1.f, 0.f}}},
		{{{1.f, 1.f, 0.f}}},
	};

	//main->GetCameraIcon();

	QColor selColor = window->GetSelectionColor();
	QColor cropColor = window->GetCropColor();
	QColor hoverColor = window->GetHoverColor();

	vec4 red;
	vec4 green;
	vec4 blue;

	vec4_set(&red, selColor.redF(), selColor.greenF(), selColor.blueF(),
		 1.0f);
	vec4_set(&green, cropColor.redF(), cropColor.greenF(),
		 cropColor.blueF(), 1.0f);
	vec4_set(&blue, hoverColor.redF(), hoverColor.greenF(),
		 hoverColor.blueF(), 1.0f);

	bool visible = std::all_of(
		std::begin(bounds), std::end(bounds), [&](const vec3 &b) {
			vec3 pos;
			vec3_transform(&pos, &b, &boxTransform);
			vec3_transform(&pos, &pos, &invBoxTransform);
			return CloseFloat(pos.x, b.x) && CloseFloat(pos.y, b.y);
		});

	if (!visible)
		return true;

	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "DrawSelectedItem");

	matrix4 curTransform;
	vec2 boxScale;
	gs_matrix_get(&curTransform);
	obs_sceneitem_get_box_scale(item, &boxScale);
	boxScale.x *= curTransform.x.x;
	boxScale.y *= curTransform.y.y;

	obs_transform_info info;
	obs_sceneitem_get_info(item, &info);

	gs_matrix_push();
	gs_matrix_mul(&boxTransform);

	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	gs_effect_t *eff = gs_get_effect();
	gs_eparam_t *colParam = gs_effect_get_param_by_name(eff, "color");

	gs_effect_set_vec4(colParam, &red);

	if (info.bounds_type == OBS_BOUNDS_NONE && crop_enabled(&crop)) {
#define DRAW_SIDE(side, x1, y1, x2, y2)                                        \
	if (hovered && !selected) {                                            \
		gs_effect_set_vec4(colParam, &blue);                           \
		DrawLine(x1, y1, x2, y2, HANDLE_RADIUS *pixelRatio / 2,        \
			 boxScale);                                            \
	} else if (crop.side > 0) {                                            \
		gs_effect_set_vec4(colParam, &green);                          \
		DrawStripedLine(x1, y1, x2, y2, HANDLE_RADIUS *pixelRatio / 2, \
				boxScale);                                     \
	} else {                                                               \
		DrawLine(x1, y1, x2, y2, HANDLE_RADIUS *pixelRatio / 2,        \
			 boxScale);                                            \
	}                                                                      \
	gs_effect_set_vec4(colParam, &red);

		DRAW_SIDE(left, 0.0f, 0.0f, 0.0f, 1.0f);
		DRAW_SIDE(top, 0.0f, 0.0f, 1.0f, 0.0f);
		DRAW_SIDE(right, 1.0f, 0.0f, 1.0f, 1.0f);
		DRAW_SIDE(bottom, 0.0f, 1.0f, 1.0f, 1.0f);
#undef DRAW_SIDE
	} else {
		if (!selected) {
			gs_effect_set_vec4(colParam, &blue);
			DrawRect(HANDLE_RADIUS * pixelRatio / 2, boxScale);
		} else {
			DrawRect(HANDLE_RADIUS * pixelRatio / 2, boxScale);
		}
	}

	gs_load_vertexbuffer(window->box);
	gs_effect_set_vec4(colParam, &red);

	if (selected) {
		DrawSquareAtPos(0.0f, 0.0f, pixelRatio);
		DrawSquareAtPos(0.0f, 1.0f, pixelRatio);
		DrawSquareAtPos(1.0f, 0.0f, pixelRatio);
		DrawSquareAtPos(1.0f, 1.0f, pixelRatio);
		DrawSquareAtPos(0.5f, 0.0f, pixelRatio);
		DrawSquareAtPos(0.0f, 0.5f, pixelRatio);
		DrawSquareAtPos(0.5f, 1.0f, pixelRatio);
		DrawSquareAtPos(1.0f, 0.5f, pixelRatio);

		if (!window->circleFill) {
			gs_render_start(true);

			float angle = 180;
			for (int i = 0, l = 40; i < l; i++) {
				gs_vertex2f(sin(RAD(angle)) / 2 + 0.5f,
					    cos(RAD(angle)) / 2 + 0.5f);
				angle += 360 / l;
				gs_vertex2f(sin(RAD(angle)) / 2 + 0.5f,
					    cos(RAD(angle)) / 2 + 0.5f);
				gs_vertex2f(0.5f, 1.0f);
			}

			window->circleFill = gs_render_save();
		}

		DrawRotationHandle(window->circleFill,
				   info.rot + window->groupRot, pixelRatio);
	}

	gs_matrix_pop();

	GS_DEBUG_MARKER_END();

	UNUSED_PARAMETER(scene);
	return true;
}

static inline QColor color_from_int(long long val)
{
	return QColor(val & 0xff, (val >> 8) & 0xff, (val >> 16) & 0xff,
		      (val >> 24) & 0xff);
}

QColor CanvasDock::GetSelectionColor() const
{
	if (config_get_bool(obs_frontend_get_global_config(), "Accessibility",
			    "OverrideColors")) {
		return color_from_int(
			config_get_int(obs_frontend_get_global_config(),
				       "Accessibility", "SelectRed"));
	}
	return QColor::fromRgb(255, 0, 0);
}

QColor CanvasDock::GetCropColor() const
{
	if (config_get_bool(obs_frontend_get_global_config(), "Accessibility",
			    "OverrideColors")) {
		return color_from_int(
			config_get_int(obs_frontend_get_global_config(),
				       "Accessibility", "SelectGreen"));
	}
	return QColor::fromRgb(0, 255, 0);
}

QColor CanvasDock::GetHoverColor() const
{
	if (config_get_bool(obs_frontend_get_global_config(), "Accessibility",
			    "OverrideColors")) {
		return color_from_int(
			config_get_int(obs_frontend_get_global_config(),
				       "Accessibility", "SelectBlue"));
	}
	return QColor::fromRgb(0, 127, 255);
}

OBSEventFilter *CanvasDock::BuildEventFilter()
{
	return new OBSEventFilter([this](QObject *obj, QEvent *event) {
		UNUSED_PARAMETER(obj);

		switch (event->type()) {
		case QEvent::MouseButtonPress:
			return this->HandleMousePressEvent(
				static_cast<QMouseEvent *>(event));
		case QEvent::MouseButtonRelease:
			return this->HandleMouseReleaseEvent(
				static_cast<QMouseEvent *>(event));
		//case QEvent::MouseButtonDblClick:			return this->HandleMouseClickEvent(				static_cast<QMouseEvent *>(event));
		case QEvent::MouseMove:
			return this->HandleMouseMoveEvent(
				static_cast<QMouseEvent *>(event));
		//case QEvent::Enter:
		case QEvent::Leave:
			return this->HandleMouseLeaveEvent(
				static_cast<QMouseEvent *>(event));
		case QEvent::Wheel:
			return this->HandleMouseWheelEvent(
				static_cast<QWheelEvent *>(event));
		//case QEvent::FocusIn:
		//case QEvent::FocusOut:
		case QEvent::KeyPress:
			return this->HandleKeyPressEvent(
				static_cast<QKeyEvent *>(event));
		case QEvent::KeyRelease:
			return this->HandleKeyReleaseEvent(
				static_cast<QKeyEvent *>(event));
		default:
			return false;
		}
	});
}

bool CanvasDock::GetSourceRelativeXY(int mouseX, int mouseY, int &relX,
				     int &relY)
{
	float pixelRatio = devicePixelRatioF();

	int mouseXscaled = (int)roundf(mouseX * pixelRatio);
	int mouseYscaled = (int)roundf(mouseY * pixelRatio);

	QSize size = preview->size() * preview->devicePixelRatioF();

	obs_source_t *s = obs_weak_source_get_source(source);
	uint32_t sourceCX = s ? obs_source_get_width(s) : 1;
	if (sourceCX <= 0)
		sourceCX = 1;
	uint32_t sourceCY = s ? obs_source_get_height(s) : 1;
	if (sourceCY <= 0)
		sourceCY = 1;

	obs_source_release(s);

	int x, y;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, size.width(), size.height(), x,
			     y, scale);

	auto newCX = scale * float(sourceCX);
	auto newCY = scale * float(sourceCY);

	auto extraCx = /*(zoom - 1.0f) **/ newCX;
	auto extraCy = /*(zoom - 1.0f) **/ newCY;

	//scale *= zoom;
	float scrollX = 0.5f;
	float scrollY = 0.5f;

	if (x > 0) {
		relX = int(float(mouseXscaled - x + extraCx * scrollX) / scale);
		relY = int(float(mouseYscaled + extraCy * scrollY) / scale);
	} else {
		relX = int(float(mouseXscaled + extraCx * scrollX) / scale);
		relY = int(float(mouseYscaled - y + extraCy * scrollY) / scale);
	}

	// Confirm mouse is inside the source
	if (relX < 0 || relX > int(sourceCX))
		return false;
	if (relY < 0 || relY > int(sourceCY))
		return false;

	return true;
}

bool CanvasDock::HandleMousePressEvent(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	QPointF pos = event->position();
#else
	QPointF pos = event->localPos();
#endif

	if (scrollMode && IsFixedScaling() &&
	    event->button() == Qt::LeftButton) {
		setCursor(Qt::ClosedHandCursor);
		scrollingFrom.x = pos.x();
		scrollingFrom.y = pos.y();
		return true;
	}

	if (event->button() == Qt::RightButton) {
		scrollMode = false;
		setCursor(Qt::ArrowCursor);
	}

	if (locked)
		return false;

	//float pixelRatio = 1.0f;
	//float x = pos.x() - main->previewX / pixelRatio;
	//float y = pos.y() - main->previewY / pixelRatio;
	Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();
	bool altDown = (modifiers & Qt::AltModifier);
	bool shiftDown = (modifiers & Qt::ShiftModifier);
	bool ctrlDown = (modifiers & Qt::ControlModifier);

	if (event->button() != Qt::LeftButton &&
	    event->button() != Qt::RightButton)
		return false;

	if (event->button() == Qt::LeftButton)
		mouseDown = true;

	{
		std::lock_guard<std::mutex> lock(selectMutex);
		selectedItems.clear();
	}

	if (altDown)
		cropping = true;

	if (altDown || shiftDown || ctrlDown) {
		vec2 s;
		SceneFindBoxData data(s, s);

		obs_scene_enum_items(scene, FindSelected, &data);

		std::lock_guard<std::mutex> lock(selectMutex);
		selectedItems = data.sceneItems;
	}
	startPos = GetMouseEventPos(event);

	//vec2_set(&startPos, mouseEvent.x, mouseEvent.y);
	//GetStretchHandleData(startPos, false);

	//vec2_divf(&startPos, &startPos, main->previewScale / pixelRatio);
	startPos.x = std::round(startPos.x);
	startPos.y = std::round(startPos.y);

	mouseOverItems = SelectedAtPos(scene, startPos);
	vec2_zero(&lastMoveOffset);

	mousePos = startPos;
	changed = false;

	return true;
}

bool CanvasDock::HandleMouseReleaseEvent(QMouseEvent *event)
{
	if (scrollMode)
		setCursor(Qt::OpenHandCursor);

	if (!mouseDown && event->button() == Qt::RightButton) {
		QMenu popup(this);
		QAction *action = popup.addAction(obs_module_text("Locked"),
						  this,
						  [this] { locked = !locked; });
		action->setCheckable(true);
		action->setChecked(locked);

		popup.addAction(obs_module_text("Screenshot"), this, [this] {
			auto s = obs_weak_source_get_source(source);
			obs_frontend_take_source_screenshot(s);
			obs_source_release(s);
		});

		popup.addMenu(CreateAddSourcePopupMenu());

		popup.addSeparator();

		OBSSceneItem sceneItem = GetSelectedItem();
		if (sceneItem) {
			obs_source_t *source =
				obs_sceneitem_get_source(sceneItem);

			popup.addAction(
				obs_module_text("Remove"), this, [sceneItem] {
					QMessageBox mb(
						QMessageBox::Question,
						obs_module_text("Delete?"),
						obs_module_text(
							"Are you sure?"),
						QMessageBox::StandardButtons(
							QMessageBox::Yes |
							QMessageBox::No));
					mb.setDefaultButton(
						QMessageBox::NoButton);
					if (mb.exec() == QMessageBox::Yes) {
						obs_sceneitem_remove(sceneItem);
					}
				});

			popup.addSeparator();
			auto orderMenu =
				popup.addMenu(obs_module_text("Order"));
			orderMenu->addAction(
				obs_module_text("Up"), this, [sceneItem] {
					obs_sceneitem_set_order(
						sceneItem, OBS_ORDER_MOVE_UP);
				});
			orderMenu->addAction(
				obs_module_text("Down"), this, [sceneItem] {
					obs_sceneitem_set_order(
						sceneItem, OBS_ORDER_MOVE_DOWN);
				});
			orderMenu->addSeparator();
			orderMenu->addAction(
				obs_module_text("Top"), this, [sceneItem] {
					obs_sceneitem_set_order(
						sceneItem, OBS_ORDER_MOVE_TOP);
				});
			orderMenu->addAction(
				obs_module_text("Bottom"), this, [sceneItem] {
					obs_sceneitem_set_order(
						sceneItem,
						OBS_ORDER_MOVE_BOTTOM);
				});

			auto transformMenu =
				popup.addMenu(obs_module_text("Transform"));
			transformMenu->addAction(
				obs_module_text("Reset"), this, [sceneItem] {
					obs_sceneitem_set_alignment(
						sceneItem,
						OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
					obs_sceneitem_set_bounds_type(
						sceneItem, OBS_BOUNDS_NONE);
					vec2 scale;
					scale.x = 1.0f;
					scale.y = 1.0f;
					obs_sceneitem_set_scale(sceneItem,
								&scale);
					vec2 pos;
					pos.x = 0.0f;
					pos.y = 0.0f;
					obs_sceneitem_set_pos(sceneItem, &pos);
					obs_sceneitem_crop crop = {0, 0, 0, 0};
					obs_sceneitem_set_crop(sceneItem,
							       &crop);
					obs_sceneitem_set_rot(sceneItem, 0.0f);
				});

			action = popup.addAction(
				obs_module_text("Locked"), this, [sceneItem] {
					obs_sceneitem_set_locked(
						sceneItem,
						!obs_sceneitem_locked(
							sceneItem));
				});
			action->setCheckable(true);
			action->setChecked(obs_sceneitem_locked(sceneItem));
			popup.addAction(
				obs_module_text("Filters"), this, [source] {
					obs_frontend_open_source_filters(
						source);
				});
			action = popup.addAction(
				obs_module_text("Properties"), this, [source] {
					obs_frontend_open_source_properties(
						source);
				});
			action->setEnabled(obs_source_configurable(source));
		}
		popup.exec(QCursor::pos());
		return true;
	}

	if (locked)
		return false;

	if (!mouseDown)
		return false;

	const vec2 pos = GetMouseEventPos(event);

	if (!mouseMoved)
		ProcessClick(pos);

	if (selectionBox) {
		Qt::KeyboardModifiers modifiers =
			QGuiApplication::keyboardModifiers();

		bool altDown = modifiers & Qt::AltModifier;
		bool shiftDown = modifiers & Qt::ShiftModifier;
		bool ctrlDown = modifiers & Qt::ControlModifier;

		std::lock_guard<std::mutex> lock(selectMutex);
		if (altDown || ctrlDown || shiftDown) {
			for (size_t i = 0; i < selectedItems.size(); i++) {
				obs_sceneitem_select(selectedItems[i], true);
			}
		}

		for (size_t i = 0; i < hoveredPreviewItems.size(); i++) {
			bool select = true;
			obs_sceneitem_t *item = hoveredPreviewItems[i];

			if (altDown) {
				select = false;
			} else if (ctrlDown) {
				select = !obs_sceneitem_selected(item);
			}

			obs_sceneitem_select(hoveredPreviewItems[i], select);
		}
	}

	if (stretchGroup) {
		obs_sceneitem_defer_group_resize_end(stretchGroup);
	}

	stretchItem = nullptr;
	stretchGroup = nullptr;
	mouseDown = false;
	mouseMoved = false;
	cropping = false;
	selectionBox = false;
	unsetCursor();

	OBSSceneItem item = GetItemAtPos(pos, true);

	std::lock_guard<std::mutex> lock(selectMutex);
	hoveredPreviewItems.clear();
	hoveredPreviewItems.push_back(item);
	selectedItems.clear();

	return true;
}

float CanvasDock::GetDevicePixelRatio()
{
	return 1.0f;
}

bool CanvasDock::HandleMouseLeaveEvent(QMouseEvent *event)
{
	UNUSED_PARAMETER(event);
	std::lock_guard<std::mutex> lock(selectMutex);
	if (!selectionBox)
		hoveredPreviewItems.clear();
	return true;
}

bool CanvasDock::HandleMouseMoveEvent(QMouseEvent *event)
{
	changed = true;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	QPointF qtPos = event->position();
#else
	QPointF qtPos = event->localPos();
#endif

	float pixelRatio = GetDevicePixelRatio();

	if (scrollMode && event->buttons() == Qt::LeftButton) {
		scrollingOffset.x += pixelRatio * (qtPos.x() - scrollingFrom.x);
		scrollingOffset.y += pixelRatio * (qtPos.y() - scrollingFrom.y);
		scrollingFrom.x = qtPos.x();
		scrollingFrom.y = qtPos.y();
		//emit DisplayResized();
		return true;
	}

	if (locked)
		return true;

	bool updateCursor = false;

	if (mouseDown) {
		vec2 pos = GetMouseEventPos(event);

		if (!mouseMoved && !mouseOverItems &&
		    stretchHandle == ItemHandle::None) {
			ProcessClick(startPos);
			mouseOverItems = SelectedAtPos(scene, startPos);
		}

		pos.x = std::round(pos.x);
		pos.y = std::round(pos.y);

		if (stretchHandle != ItemHandle::None) {
			if (obs_sceneitem_locked(stretchItem))
				return true;

			selectionBox = false;

			obs_sceneitem_t *group =
				obs_sceneitem_get_group(scene, stretchItem);
			if (group) {
				vec3 group_pos;
				vec3_set(&group_pos, pos.x, pos.y, 0.0f);
				vec3_transform(&group_pos, &group_pos,
					       &invGroupTransform);
				pos.x = group_pos.x;
				pos.y = group_pos.y;
			}

			if (stretchHandle == ItemHandle::Rot) {
				RotateItem(pos);
				setCursor(Qt::ClosedHandCursor);
			} else if (cropping)
				CropItem(pos);
			else
				StretchItem(pos);

		} else if (mouseOverItems) {
			if (cursor().shape() != Qt::SizeAllCursor)
				setCursor(Qt::SizeAllCursor);
			selectionBox = false;
			MoveItems(pos);
		} else {
			selectionBox = true;
			if (!mouseMoved)
				DoSelect(startPos);
			BoxItems(startPos, pos);
		}

		mouseMoved = true;
		mousePos = pos;
	} else {
		vec2 pos = GetMouseEventPos(event);
		OBSSceneItem item = GetItemAtPos(pos, true);

		std::lock_guard<std::mutex> lock(selectMutex);
		hoveredPreviewItems.clear();
		hoveredPreviewItems.push_back(item);

		if (!mouseMoved && hoveredPreviewItems.size() > 0) {
			mousePos = pos;
			//float scale = GetDevicePixelRatio();
			//float x = qtPos.x(); // - main->previewX / scale;
			//float y = qtPos.y(); // - main->previewY / scale;
			vec2_set(&startPos, pos.x, pos.y);
			updateCursor = true;
		}
	}

	if (updateCursor) {
		GetStretchHandleData(startPos, true);
		uint32_t stretchFlags = (uint32_t)stretchHandle;
		UpdateCursor(stretchFlags);
	}
	return true;
}
bool CanvasDock::HandleMouseWheelEvent(QWheelEvent *event)
{
	UNUSED_PARAMETER(event);
	return true;
}

bool CanvasDock::HandleKeyPressEvent(QKeyEvent *event)
{
	UNUSED_PARAMETER(event);
	return true;
}

bool CanvasDock::HandleKeyReleaseEvent(QKeyEvent *event)
{
	UNUSED_PARAMETER(event);
	return true;
}

static bool CheckItemSelected(obs_scene_t *scene, obs_sceneitem_t *item,
			      void *param)
{
	SceneFindData *data = reinterpret_cast<SceneFindData *>(param);
	matrix4 transform;
	vec3 transformedPos;
	vec3 pos3;

	if (!SceneItemHasVideo(item))
		return true;
	if (obs_sceneitem_is_group(item)) {
		data->group = item;
		obs_sceneitem_group_enum_items(item, CheckItemSelected, param);
		data->group = nullptr;

		if (data->item) {
			return false;
		}
	}

	vec3_set(&pos3, data->pos.x, data->pos.y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	if (data->group) {
		matrix4 parent_transform;
		obs_sceneitem_get_draw_transform(data->group,
						 &parent_transform);
		matrix4_mul(&transform, &transform, &parent_transform);
	}

	matrix4_inv(&transform, &transform);
	vec3_transform(&transformedPos, &pos3, &transform);

	if (transformedPos.x >= 0.0f && transformedPos.x <= 1.0f &&
	    transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {
		if (obs_sceneitem_selected(item)) {
			data->item = item;
			return false;
		}
	}

	UNUSED_PARAMETER(scene);
	return true;
}

bool CanvasDock::SelectedAtPos(obs_scene_t *scene, const vec2 &pos)
{
	if (!scene)
		return false;

	SceneFindData data(pos, false);
	obs_scene_enum_items(scene, CheckItemSelected, &data);
	return !!data.item;
}

static bool select_one(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	obs_sceneitem_t *selectedItem =
		reinterpret_cast<obs_sceneitem_t *>(param);
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, select_one, param);

	obs_sceneitem_select(item, (selectedItem == item));

	UNUSED_PARAMETER(scene);
	return true;
}

void CanvasDock::DoSelect(const vec2 &pos)
{
	OBSSceneItem item = GetItemAtPos(pos, true);
	obs_scene_enum_items(scene, select_one, (obs_sceneitem_t *)item);
}

void CanvasDock::DoCtrlSelect(const vec2 &pos)
{
	OBSSceneItem item = GetItemAtPos(pos, false);
	if (!item)
		return;

	bool selected = obs_sceneitem_selected(item);
	obs_sceneitem_select(item, !selected);
}

void CanvasDock::ProcessClick(const vec2 &pos)
{
	Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();

	if (modifiers & Qt::ControlModifier)
		DoCtrlSelect(pos);
	else
		DoSelect(pos);
}

static bool FindItemAtPos(obs_scene_t *scene, obs_sceneitem_t *item,
			  void *param)
{
	SceneFindData *data = reinterpret_cast<SceneFindData *>(param);
	matrix4 transform;
	matrix4 invTransform;
	vec3 transformedPos;
	vec3 pos3;
	vec3 pos3_;

	if (!SceneItemHasVideo(item))
		return true;
	if (obs_sceneitem_locked(item))
		return true;

	vec3_set(&pos3, data->pos.x, data->pos.y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	matrix4_inv(&invTransform, &transform);
	vec3_transform(&transformedPos, &pos3, &invTransform);
	vec3_transform(&pos3_, &transformedPos, &transform);

	if (CloseFloat(pos3.x, pos3_.x) && CloseFloat(pos3.y, pos3_.y) &&
	    transformedPos.x >= 0.0f && transformedPos.x <= 1.0f &&
	    transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {
		if (data->selectBelow && obs_sceneitem_selected(item)) {
			if (data->item)
				return false;
			else
				data->selectBelow = false;
		}

		data->item = item;
	}

	UNUSED_PARAMETER(scene);
	return true;
}

OBSSceneItem CanvasDock::GetItemAtPos(const vec2 &pos, bool selectBelow)
{
	if (!scene)
		return OBSSceneItem();

	SceneFindData data(pos, selectBelow);
	obs_scene_enum_items(scene, FindItemAtPos, &data);
	return data.item;
}

vec2 CanvasDock::GetMouseEventPos(QMouseEvent *event)
{

	auto source = obs_weak_source_get_source(this->source);
	uint32_t sourceCX = obs_source_get_width(source);
	if (sourceCX <= 0)
		sourceCX = 1;
	uint32_t sourceCY = obs_source_get_height(source);
	if (sourceCY <= 0)
		sourceCY = 1;
	obs_source_release(source);

	int x, y;
	float scale;

	auto size = preview->size();

	GetScaleAndCenterPos(sourceCX, sourceCY, size.width(), size.height(), x,
			     y, scale);
	//auto newCX = scale * float(sourceCX);
	//auto newCY = scale * float(sourceCY);
	float pixelRatio = 1.0f; //main->GetDevicePixelRatio();

	QPoint qtPos = event->pos();

	vec2 pos;
	vec2_set(&pos, (qtPos.x() - x / pixelRatio) / scale,
		 (qtPos.y() - y / pixelRatio) / scale);

	return pos;
}

void CanvasDock::UpdateCursor(uint32_t &flags)
{
	if (obs_sceneitem_locked(stretchItem)) {
		unsetCursor();
		return;
	}

	if (!flags && (cursor().shape() != Qt::OpenHandCursor || !scrollMode))
		unsetCursor();
	if (cursor().shape() != Qt::ArrowCursor)
		return;

	if ((flags & ITEM_LEFT && flags & ITEM_TOP) ||
	    (flags & ITEM_RIGHT && flags & ITEM_BOTTOM))
		setCursor(Qt::SizeFDiagCursor);
	else if ((flags & ITEM_LEFT && flags & ITEM_BOTTOM) ||
		 (flags & ITEM_RIGHT && flags & ITEM_TOP))
		setCursor(Qt::SizeBDiagCursor);
	else if (flags & ITEM_LEFT || flags & ITEM_RIGHT)
		setCursor(Qt::SizeHorCursor);
	else if (flags & ITEM_TOP || flags & ITEM_BOTTOM)
		setCursor(Qt::SizeVerCursor);
	else if (flags & ITEM_ROT)
		setCursor(Qt::OpenHandCursor);
}

static void RotatePos(vec2 *pos, float rot)
{
	float cosR = cos(rot);
	float sinR = sin(rot);

	vec2 newPos;

	newPos.x = cosR * pos->x - sinR * pos->y;
	newPos.y = sinR * pos->x + cosR * pos->y;

	vec2_copy(pos, &newPos);
}

void CanvasDock::RotateItem(const vec2 &pos)
{
	Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();
	bool shiftDown = (modifiers & Qt::ShiftModifier);
	bool ctrlDown = (modifiers & Qt::ControlModifier);

	vec2 pos2;
	vec2_copy(&pos2, &pos);

	float angle =
		atan2(pos2.y - rotatePoint.y, pos2.x - rotatePoint.x) + RAD(90);

#define ROT_SNAP(rot, thresh)                      \
	if (abs(angle - RAD(rot)) < RAD(thresh)) { \
		angle = RAD(rot);                  \
	}

	if (shiftDown) {
		for (int i = 0; i <= 360 / 15; i++) {
			ROT_SNAP(i * 15 - 90, 7.5);
		}
	} else if (!ctrlDown) {
		ROT_SNAP(rotateAngle, 5)

		ROT_SNAP(-90, 5)
		ROT_SNAP(-45, 5)
		ROT_SNAP(0, 5)
		ROT_SNAP(45, 5)
		ROT_SNAP(90, 5)
		ROT_SNAP(135, 5)
		ROT_SNAP(180, 5)
		ROT_SNAP(225, 5)
		ROT_SNAP(270, 5)
		ROT_SNAP(315, 5)
	}
#undef ROT_SNAP

	vec2 pos3;
	vec2_copy(&pos3, &offsetPoint);
	RotatePos(&pos3, angle);
	pos3.x += rotatePoint.x;
	pos3.y += rotatePoint.y;

	obs_sceneitem_set_rot(stretchItem, DEG(angle));
	obs_sceneitem_set_pos(stretchItem, &pos3);
}

static float maxfunc(float x, float y)
{
	return x > y ? x : y;
}

static float minfunc(float x, float y)
{
	return x < y ? x : y;
}

void CanvasDock::CropItem(const vec2 &pos)
{
	obs_bounds_type boundsType = obs_sceneitem_get_bounds_type(stretchItem);
	uint32_t stretchFlags = (uint32_t)stretchHandle;
	uint32_t align = obs_sceneitem_get_alignment(stretchItem);
	vec3 tl, br, pos3;

	vec3_zero(&tl);
	vec3_set(&br, stretchItemSize.x, stretchItemSize.y, 0.0f);

	vec3_set(&pos3, pos.x, pos.y, 0.0f);
	vec3_transform(&pos3, &pos3, &screenToItem);

	obs_sceneitem_crop crop = startCrop;
	vec2 scale;

	obs_sceneitem_get_scale(stretchItem, &scale);

	vec2 max_tl;
	vec2 max_br;

	vec2_set(&max_tl, float(-crop.left) * scale.x,
		 float(-crop.top) * scale.y);
	vec2_set(&max_br, stretchItemSize.x + crop.right * scale.x,
		 stretchItemSize.y + crop.bottom * scale.y);

	typedef std::function<float(float, float)> minmax_func_t;

	minmax_func_t min_x = scale.x < 0.0f ? maxfunc : minfunc;
	minmax_func_t min_y = scale.y < 0.0f ? maxfunc : minfunc;
	minmax_func_t max_x = scale.x < 0.0f ? minfunc : maxfunc;
	minmax_func_t max_y = scale.y < 0.0f ? minfunc : maxfunc;

	pos3.x = min_x(pos3.x, max_br.x);
	pos3.x = max_x(pos3.x, max_tl.x);
	pos3.y = min_y(pos3.y, max_br.y);
	pos3.y = max_y(pos3.y, max_tl.y);

	if (stretchFlags & ITEM_LEFT) {
		float maxX = stretchItemSize.x - (2.0 * scale.x);
		pos3.x = tl.x = min_x(pos3.x, maxX);

	} else if (stretchFlags & ITEM_RIGHT) {
		float minX = (2.0 * scale.x);
		pos3.x = br.x = max_x(pos3.x, minX);
	}

	if (stretchFlags & ITEM_TOP) {
		float maxY = stretchItemSize.y - (2.0 * scale.y);
		pos3.y = tl.y = min_y(pos3.y, maxY);

	} else if (stretchFlags & ITEM_BOTTOM) {
		float minY = (2.0 * scale.y);
		pos3.y = br.y = max_y(pos3.y, minY);
	}

#define ALIGN_X (ITEM_LEFT | ITEM_RIGHT)
#define ALIGN_Y (ITEM_TOP | ITEM_BOTTOM)
	vec3 newPos;
	vec3_zero(&newPos);

	uint32_t align_x = (align & ALIGN_X);
	uint32_t align_y = (align & ALIGN_Y);
	if (align_x == (stretchFlags & ALIGN_X) && align_x != 0)
		newPos.x = pos3.x;
	else if (align & ITEM_RIGHT)
		newPos.x = stretchItemSize.x;
	else if (!(align & ITEM_LEFT))
		newPos.x = stretchItemSize.x * 0.5f;

	if (align_y == (stretchFlags & ALIGN_Y) && align_y != 0)
		newPos.y = pos3.y;
	else if (align & ITEM_BOTTOM)
		newPos.y = stretchItemSize.y;
	else if (!(align & ITEM_TOP))
		newPos.y = stretchItemSize.y * 0.5f;
#undef ALIGN_X
#undef ALIGN_Y

	crop = startCrop;

	if (stretchFlags & ITEM_LEFT)
		crop.left += int(std::round(tl.x / scale.x));
	else if (stretchFlags & ITEM_RIGHT)
		crop.right +=
			int(std::round((stretchItemSize.x - br.x) / scale.x));

	if (stretchFlags & ITEM_TOP)
		crop.top += int(std::round(tl.y / scale.y));
	else if (stretchFlags & ITEM_BOTTOM)
		crop.bottom +=
			int(std::round((stretchItemSize.y - br.y) / scale.y));

	vec3_transform(&newPos, &newPos, &itemToScreen);
	newPos.x = std::round(newPos.x);
	newPos.y = std::round(newPos.y);

#if 0
	vec3 curPos;
	vec3_zero(&curPos);
	obs_sceneitem_get_pos(stretchItem, (vec2*)&curPos);
	blog(LOG_DEBUG, "curPos {%d, %d} - newPos {%d, %d}",
			int(curPos.x), int(curPos.y),
			int(newPos.x), int(newPos.y));
	blog(LOG_DEBUG, "crop {%d, %d, %d, %d}",
			crop.left, crop.top,
			crop.right, crop.bottom);
#endif

	obs_sceneitem_defer_update_begin(stretchItem);
	obs_sceneitem_set_crop(stretchItem, &crop);
	if (boundsType == OBS_BOUNDS_NONE)
		obs_sceneitem_set_pos(stretchItem, (vec2 *)&newPos);
	obs_sceneitem_defer_update_end(stretchItem);
}

void CanvasDock::StretchItem(const vec2 &pos)
{
	Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();
	obs_bounds_type boundsType = obs_sceneitem_get_bounds_type(stretchItem);
	uint32_t stretchFlags = (uint32_t)stretchHandle;
	bool shiftDown = (modifiers & Qt::ShiftModifier);
	vec3 tl, br, pos3;

	vec3_zero(&tl);
	vec3_set(&br, stretchItemSize.x, stretchItemSize.y, 0.0f);

	vec3_set(&pos3, pos.x, pos.y, 0.0f);
	vec3_transform(&pos3, &pos3, &screenToItem);

	if (stretchFlags & ITEM_LEFT)
		tl.x = pos3.x;
	else if (stretchFlags & ITEM_RIGHT)
		br.x = pos3.x;

	if (stretchFlags & ITEM_TOP)
		tl.y = pos3.y;
	else if (stretchFlags & ITEM_BOTTOM)
		br.y = pos3.y;

	if (!(modifiers & Qt::ControlModifier))
		SnapStretchingToScreen(tl, br);

	obs_source_t *source = obs_sceneitem_get_source(stretchItem);

	vec2 baseSize;
	vec2_set(&baseSize, float(obs_source_get_width(source)),
		 float(obs_source_get_height(source)));

	vec2 size;
	vec2_set(&size, br.x - tl.x, br.y - tl.y);

	if (boundsType != OBS_BOUNDS_NONE) {
		if (shiftDown)
			ClampAspect(tl, br, size, baseSize);

		if (tl.x > br.x)
			std::swap(tl.x, br.x);
		if (tl.y > br.y)
			std::swap(tl.y, br.y);

		vec2_abs(&size, &size);

		obs_sceneitem_set_bounds(stretchItem, &size);
	} else {
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(stretchItem, &crop);

		baseSize.x -= float(crop.left + crop.right);
		baseSize.y -= float(crop.top + crop.bottom);

		if (!shiftDown)
			ClampAspect(tl, br, size, baseSize);

		vec2_div(&size, &size, &baseSize);
		obs_sceneitem_set_scale(stretchItem, &size);
	}

	pos3 = CalculateStretchPos(tl, br);
	vec3_transform(&pos3, &pos3, &itemToScreen);

	vec2 newPos;
	vec2_set(&newPos, std::round(pos3.x), std::round(pos3.y));
	obs_sceneitem_set_pos(stretchItem, &newPos);
}

void CanvasDock::SnapStretchingToScreen(vec3 &tl, vec3 &br)
{
	uint32_t stretchFlags = (uint32_t)stretchHandle;
	vec3 newTL = GetTransformedPos(tl.x, tl.y, itemToScreen);
	vec3 newTR = GetTransformedPos(br.x, tl.y, itemToScreen);
	vec3 newBL = GetTransformedPos(tl.x, br.y, itemToScreen);
	vec3 newBR = GetTransformedPos(br.x, br.y, itemToScreen);
	vec3 boundingTL;
	vec3 boundingBR;

	vec3_copy(&boundingTL, &newTL);
	vec3_min(&boundingTL, &boundingTL, &newTR);
	vec3_min(&boundingTL, &boundingTL, &newBL);
	vec3_min(&boundingTL, &boundingTL, &newBR);

	vec3_copy(&boundingBR, &newTL);
	vec3_max(&boundingBR, &boundingBR, &newTR);
	vec3_max(&boundingBR, &boundingBR, &newBL);
	vec3_max(&boundingBR, &boundingBR, &newBR);

	vec3 offset = GetSnapOffset(boundingTL, boundingBR);
	vec3_add(&offset, &offset, &newTL);
	vec3_transform(&offset, &offset, &screenToItem);
	vec3_sub(&offset, &offset, &tl);

	if (stretchFlags & ITEM_LEFT)
		tl.x += offset.x;
	else if (stretchFlags & ITEM_RIGHT)
		br.x += offset.x;

	if (stretchFlags & ITEM_TOP)
		tl.y += offset.y;
	else if (stretchFlags & ITEM_BOTTOM)
		br.y += offset.y;
}

vec3 CanvasDock::GetSnapOffset(const vec3 &tl, const vec3 &br)
{
	auto s = obs_weak_source_get_source(source);
	vec2 screenSize;
	screenSize.x = obs_source_get_base_width(s);
	screenSize.y = obs_source_get_base_height(s);
	obs_source_release(s);
	vec3 clampOffset;

	vec3_zero(&clampOffset);

	const bool snap = config_get_bool(obs_frontend_get_global_config(),
					  "BasicWindow", "SnappingEnabled");
	if (snap == false)
		return clampOffset;

	const bool screenSnap =
		config_get_bool(obs_frontend_get_global_config(), "BasicWindow",
				"ScreenSnapping");
	const bool centerSnap =
		config_get_bool(obs_frontend_get_global_config(), "BasicWindow",
				"CenterSnapping");

	const float clampDist = config_get_double(
		obs_frontend_get_global_config(), "BasicWindow",
		"SnapDistance") /* / main->previewScale */;
	const float centerX = br.x - (br.x - tl.x) / 2.0f;
	const float centerY = br.y - (br.y - tl.y) / 2.0f;

	// Left screen edge.
	if (screenSnap && fabsf(tl.x) < clampDist)
		clampOffset.x = -tl.x;
	// Right screen edge.
	if (screenSnap && fabsf(clampOffset.x) < EPSILON &&
	    fabsf(screenSize.x - br.x) < clampDist)
		clampOffset.x = screenSize.x - br.x;
	// Horizontal center.
	if (centerSnap && fabsf(screenSize.x - (br.x - tl.x)) > clampDist &&
	    fabsf(screenSize.x / 2.0f - centerX) < clampDist)
		clampOffset.x = screenSize.x / 2.0f - centerX;

	// Top screen edge.
	if (screenSnap && fabsf(tl.y) < clampDist)
		clampOffset.y = -tl.y;
	// Bottom screen edge.
	if (screenSnap && fabsf(clampOffset.y) < EPSILON &&
	    fabsf(screenSize.y - br.y) < clampDist)
		clampOffset.y = screenSize.y - br.y;
	// Vertical center.
	if (centerSnap && fabsf(screenSize.y - (br.y - tl.y)) > clampDist &&
	    fabsf(screenSize.y / 2.0f - centerY) < clampDist)
		clampOffset.y = screenSize.y / 2.0f - centerY;

	return clampOffset;
}

static bool move_items(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	if (obs_sceneitem_locked(item))
		return true;

	bool selected = obs_sceneitem_selected(item);
	vec2 *offset = reinterpret_cast<vec2 *>(param);

	if (obs_sceneitem_is_group(item) && !selected) {
		matrix4 transform;
		vec3 new_offset;
		vec3_set(&new_offset, offset->x, offset->y, 0.0f);

		obs_sceneitem_get_draw_transform(item, &transform);
		vec4_set(&transform.t, 0.0f, 0.0f, 0.0f, 1.0f);
		matrix4_inv(&transform, &transform);
		vec3_transform(&new_offset, &new_offset, &transform);
		obs_sceneitem_group_enum_items(item, move_items, &new_offset);
	}

	if (selected) {
		vec2 pos;
		obs_sceneitem_get_pos(item, &pos);
		vec2_add(&pos, &pos, offset);
		obs_sceneitem_set_pos(item, &pos);
	}

	UNUSED_PARAMETER(scene);
	return true;
}

void CanvasDock::MoveItems(const vec2 &pos)
{
	Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();

	vec2 offset, moveOffset;
	vec2_sub(&offset, &pos, &startPos);
	vec2_sub(&moveOffset, &offset, &lastMoveOffset);

	if (!(modifiers & Qt::ControlModifier))
		SnapItemMovement(moveOffset);

	vec2_add(&lastMoveOffset, &lastMoveOffset, &moveOffset);

	obs_scene_enum_items(scene, move_items, &moveOffset);
}

struct SelectedItemBounds {
	bool first = true;
	vec3 tl, br;
};

static bool AddItemBounds(obs_scene_t *scene, obs_sceneitem_t *item,
			  void *param)
{
	SelectedItemBounds *data =
		reinterpret_cast<SelectedItemBounds *>(param);
	vec3 t[4];

	auto add_bounds = [data, &t]() {
		for (const vec3 &v : t) {
			if (data->first) {
				vec3_copy(&data->tl, &v);
				vec3_copy(&data->br, &v);
				data->first = false;
			} else {
				vec3_min(&data->tl, &data->tl, &v);
				vec3_max(&data->br, &data->br, &v);
			}
		}
	};

	if (obs_sceneitem_is_group(item)) {
		SelectedItemBounds sib;
		obs_sceneitem_group_enum_items(item, AddItemBounds, &sib);

		if (!sib.first) {
			matrix4 xform;
			obs_sceneitem_get_draw_transform(item, &xform);

			vec3_set(&t[0], sib.tl.x, sib.tl.y, 0.0f);
			vec3_set(&t[1], sib.tl.x, sib.br.y, 0.0f);
			vec3_set(&t[2], sib.br.x, sib.tl.y, 0.0f);
			vec3_set(&t[3], sib.br.x, sib.br.y, 0.0f);
			vec3_transform(&t[0], &t[0], &xform);
			vec3_transform(&t[1], &t[1], &xform);
			vec3_transform(&t[2], &t[2], &xform);
			vec3_transform(&t[3], &t[3], &xform);
			add_bounds();
		}
	}
	if (!obs_sceneitem_selected(item))
		return true;

	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	t[0] = GetTransformedPos(0.0f, 0.0f, boxTransform);
	t[1] = GetTransformedPos(1.0f, 0.0f, boxTransform);
	t[2] = GetTransformedPos(0.0f, 1.0f, boxTransform);
	t[3] = GetTransformedPos(1.0f, 1.0f, boxTransform);
	add_bounds();

	UNUSED_PARAMETER(scene);
	return true;
}

struct OffsetData {
	float clampDist;
	vec3 tl, br, offset;
};

static bool GetSourceSnapOffset(obs_scene_t *scene, obs_sceneitem_t *item,
				void *param)
{
	OffsetData *data = reinterpret_cast<OffsetData *>(param);

	if (obs_sceneitem_selected(item))
		return true;

	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	vec3 t[4] = {GetTransformedPos(0.0f, 0.0f, boxTransform),
		     GetTransformedPos(1.0f, 0.0f, boxTransform),
		     GetTransformedPos(0.0f, 1.0f, boxTransform),
		     GetTransformedPos(1.0f, 1.0f, boxTransform)};

	bool first = true;
	vec3 tl, br;
	vec3_zero(&tl);
	vec3_zero(&br);
	for (const vec3 &v : t) {
		if (first) {
			vec3_copy(&tl, &v);
			vec3_copy(&br, &v);
			first = false;
		} else {
			vec3_min(&tl, &tl, &v);
			vec3_max(&br, &br, &v);
		}
	}

	// Snap to other source edges
#define EDGE_SNAP(l, r, x, y)                                               \
	do {                                                                \
		double dist = fabsf(l.x - data->r.x);                       \
		if (dist < data->clampDist &&                               \
		    fabsf(data->offset.x) < EPSILON && data->tl.y < br.y && \
		    data->br.y > tl.y &&                                    \
		    (fabsf(data->offset.x) > dist ||                        \
		     data->offset.x < EPSILON))                             \
			data->offset.x = l.x - data->r.x;                   \
	} while (false)

	EDGE_SNAP(tl, br, x, y);
	EDGE_SNAP(tl, br, y, x);
	EDGE_SNAP(br, tl, x, y);
	EDGE_SNAP(br, tl, y, x);
#undef EDGE_SNAP

	UNUSED_PARAMETER(scene);
	return true;
}

void CanvasDock::SnapItemMovement(vec2 &offset)
{
	SelectedItemBounds data;
	obs_scene_enum_items(scene, AddItemBounds, &data);

	data.tl.x += offset.x;
	data.tl.y += offset.y;
	data.br.x += offset.x;
	data.br.y += offset.y;

	vec3 snapOffset = GetSnapOffset(data.tl, data.br);

	const bool snap = config_get_bool(obs_frontend_get_global_config(),
					  "BasicWindow", "SnappingEnabled");
	const bool sourcesSnap =
		config_get_bool(obs_frontend_get_global_config(), "BasicWindow",
				"SourceSnapping");
	if (snap == false)
		return;
	if (sourcesSnap == false) {
		offset.x += snapOffset.x;
		offset.y += snapOffset.y;
		return;
	}

	const float clampDist = config_get_double(
		obs_frontend_get_global_config(), "BasicWindow",
		"SnapDistance") /* /	main->previewScale */;

	OffsetData offsetData;
	offsetData.clampDist = clampDist;
	offsetData.tl = data.tl;
	offsetData.br = data.br;
	vec3_copy(&offsetData.offset, &snapOffset);

	obs_scene_enum_items(scene, GetSourceSnapOffset, &offsetData);

	if (fabsf(offsetData.offset.x) > EPSILON ||
	    fabsf(offsetData.offset.y) > EPSILON) {
		offset.x += offsetData.offset.x;
		offset.y += offsetData.offset.y;
	} else {
		offset.x += snapOffset.x;
		offset.y += snapOffset.y;
	}
}

static bool CounterClockwise(float x1, float x2, float x3, float y1, float y2,
			     float y3)
{
	return (y3 - y1) * (x2 - x1) > (y2 - y1) * (x3 - x1);
}

static bool IntersectLine(float x1, float x2, float x3, float x4, float y1,
			  float y2, float y3, float y4)
{
	bool a = CounterClockwise(x1, x2, x3, y1, y2, y3);
	bool b = CounterClockwise(x1, x2, x4, y1, y2, y4);
	bool c = CounterClockwise(x3, x4, x1, y3, y4, y1);
	bool d = CounterClockwise(x3, x4, x2, y3, y4, y2);

	return (a != b) && (c != d);
}

static bool IntersectBox(matrix4 transform, float x1, float x2, float y1,
			 float y2)
{
	float x3, x4, y3, y4;

	x3 = transform.t.x;
	y3 = transform.t.y;
	x4 = x3 + transform.x.x;
	y4 = y3 + transform.x.y;

	if (IntersectLine(x1, x1, x3, x4, y1, y2, y3, y4) ||
	    IntersectLine(x1, x2, x3, x4, y1, y1, y3, y4) ||
	    IntersectLine(x2, x2, x3, x4, y1, y2, y3, y4) ||
	    IntersectLine(x1, x2, x3, x4, y2, y2, y3, y4))
		return true;

	x4 = x3 + transform.y.x;
	y4 = y3 + transform.y.y;

	if (IntersectLine(x1, x1, x3, x4, y1, y2, y3, y4) ||
	    IntersectLine(x1, x2, x3, x4, y1, y1, y3, y4) ||
	    IntersectLine(x2, x2, x3, x4, y1, y2, y3, y4) ||
	    IntersectLine(x1, x2, x3, x4, y2, y2, y3, y4))
		return true;

	x3 = transform.t.x + transform.x.x;
	y3 = transform.t.y + transform.x.y;
	x4 = x3 + transform.y.x;
	y4 = y3 + transform.y.y;

	if (IntersectLine(x1, x1, x3, x4, y1, y2, y3, y4) ||
	    IntersectLine(x1, x2, x3, x4, y1, y1, y3, y4) ||
	    IntersectLine(x2, x2, x3, x4, y1, y2, y3, y4) ||
	    IntersectLine(x1, x2, x3, x4, y2, y2, y3, y4))
		return true;

	x3 = transform.t.x + transform.y.x;
	y3 = transform.t.y + transform.y.y;
	x4 = x3 + transform.x.x;
	y4 = y3 + transform.x.y;

	if (IntersectLine(x1, x1, x3, x4, y1, y2, y3, y4) ||
	    IntersectLine(x1, x2, x3, x4, y1, y1, y3, y4) ||
	    IntersectLine(x2, x2, x3, x4, y1, y2, y3, y4) ||
	    IntersectLine(x1, x2, x3, x4, y2, y2, y3, y4))
		return true;

	return false;
}

static bool FindItemsInBox(obs_scene_t *scene, obs_sceneitem_t *item,
			   void *param)
{
	SceneFindBoxData *data = reinterpret_cast<SceneFindBoxData *>(param);
	matrix4 transform;
	matrix4 invTransform;
	vec3 transformedPos;
	vec3 pos3;
	vec3 pos3_;

	vec2 pos_min, pos_max;
	vec2_min(&pos_min, &data->startPos, &data->pos);
	vec2_max(&pos_max, &data->startPos, &data->pos);

	const float x1 = pos_min.x;
	const float x2 = pos_max.x;
	const float y1 = pos_min.y;
	const float y2 = pos_max.y;

	if (!SceneItemHasVideo(item))
		return true;
	if (obs_sceneitem_locked(item))
		return true;
	if (!obs_sceneitem_visible(item))
		return true;

	vec3_set(&pos3, data->pos.x, data->pos.y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	matrix4_inv(&invTransform, &transform);
	vec3_transform(&transformedPos, &pos3, &invTransform);
	vec3_transform(&pos3_, &transformedPos, &transform);

	if (CloseFloat(pos3.x, pos3_.x) && CloseFloat(pos3.y, pos3_.y) &&
	    transformedPos.x >= 0.0f && transformedPos.x <= 1.0f &&
	    transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (transform.t.x > x1 && transform.t.x < x2 && transform.t.y > y1 &&
	    transform.t.y < y2) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (transform.t.x + transform.x.x > x1 &&
	    transform.t.x + transform.x.x < x2 &&
	    transform.t.y + transform.x.y > y1 &&
	    transform.t.y + transform.x.y < y2) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (transform.t.x + transform.y.x > x1 &&
	    transform.t.x + transform.y.x < x2 &&
	    transform.t.y + transform.y.y > y1 &&
	    transform.t.y + transform.y.y < y2) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (transform.t.x + transform.x.x + transform.y.x > x1 &&
	    transform.t.x + transform.x.x + transform.y.x < x2 &&
	    transform.t.y + transform.x.y + transform.y.y > y1 &&
	    transform.t.y + transform.x.y + transform.y.y < y2) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (transform.t.x + 0.5 * (transform.x.x + transform.y.x) > x1 &&
	    transform.t.x + 0.5 * (transform.x.x + transform.y.x) < x2 &&
	    transform.t.y + 0.5 * (transform.x.y + transform.y.y) > y1 &&
	    transform.t.y + 0.5 * (transform.x.y + transform.y.y) < y2) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (IntersectBox(transform, x1, x2, y1, y2)) {
		data->sceneItems.push_back(item);
		return true;
	}

	UNUSED_PARAMETER(scene);
	return true;
}

void CanvasDock::BoxItems(const vec2 &startPos, const vec2 &pos)
{
	if (!scene)
		return;

	if (cursor().shape() != Qt::CrossCursor)
		setCursor(Qt::CrossCursor);

	SceneFindBoxData data(startPos, pos);
	obs_scene_enum_items(scene, FindItemsInBox, &data);

	std::lock_guard<std::mutex> lock(selectMutex);
	hoveredPreviewItems = data.sceneItems;
}

struct HandleFindData {
	const vec2 &pos;
	const float radius;
	matrix4 parent_xform;

	OBSSceneItem item;
	ItemHandle handle = ItemHandle::None;
	float angle = 0.0f;
	vec2 rotatePoint;
	vec2 offsetPoint;

	float angleOffset = 0.0f;

	HandleFindData(const HandleFindData &) = delete;
	HandleFindData(HandleFindData &&) = delete;
	HandleFindData &operator=(const HandleFindData &) = delete;
	HandleFindData &operator=(HandleFindData &&) = delete;

	inline HandleFindData(const vec2 &pos_, float scale)
		: pos(pos_), radius(HANDLE_SEL_RADIUS / scale)
	{
		matrix4_identity(&parent_xform);
	}

	inline HandleFindData(const HandleFindData &hfd,
			      obs_sceneitem_t *parent)
		: pos(hfd.pos),
		  radius(hfd.radius),
		  item(hfd.item),
		  handle(hfd.handle),
		  angle(hfd.angle),
		  rotatePoint(hfd.rotatePoint),
		  offsetPoint(hfd.offsetPoint)
	{
		obs_sceneitem_get_draw_transform(parent, &parent_xform);
	}
};

static bool FindHandleAtPos(obs_scene_t *scene, obs_sceneitem_t *item,
			    void *param)
{
	HandleFindData &data = *reinterpret_cast<HandleFindData *>(param);

	if (!obs_sceneitem_selected(item)) {
		if (obs_sceneitem_is_group(item)) {
			HandleFindData newData(data, item);
			newData.angleOffset = obs_sceneitem_get_rot(item);

			obs_sceneitem_group_enum_items(item, FindHandleAtPos,
						       &newData);

			data.item = newData.item;
			data.handle = newData.handle;
			data.angle = newData.angle;
			data.rotatePoint = newData.rotatePoint;
			data.offsetPoint = newData.offsetPoint;
		}

		return true;
	}

	matrix4 transform;
	vec3 pos3;
	float closestHandle = data.radius;

	vec3_set(&pos3, data.pos.x, data.pos.y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	auto TestHandle = [&](float x, float y, ItemHandle handle) {
		vec3 handlePos = GetTransformedPos(x, y, transform);
		vec3_transform(&handlePos, &handlePos, &data.parent_xform);

		float dist = vec3_dist(&handlePos, &pos3);
		if (dist < data.radius) {
			if (dist < closestHandle) {
				closestHandle = dist;
				data.handle = handle;
				data.item = item;
			}
		}
	};

	TestHandle(0.0f, 0.0f, ItemHandle::TopLeft);
	TestHandle(0.5f, 0.0f, ItemHandle::TopCenter);
	TestHandle(1.0f, 0.0f, ItemHandle::TopRight);
	TestHandle(0.0f, 0.5f, ItemHandle::CenterLeft);
	TestHandle(1.0f, 0.5f, ItemHandle::CenterRight);
	TestHandle(0.0f, 1.0f, ItemHandle::BottomLeft);
	TestHandle(0.5f, 1.0f, ItemHandle::BottomCenter);
	TestHandle(1.0f, 1.0f, ItemHandle::BottomRight);

	vec2 rotHandleOffset;
	vec2_set(&rotHandleOffset, 0.0f,
		 HANDLE_RADIUS * data.radius * 1.5 - data.radius);
	RotatePos(&rotHandleOffset, atan2(transform.x.y, transform.x.x));
	RotatePos(&rotHandleOffset, RAD(data.angleOffset));

	vec3 handlePos = GetTransformedPos(0.5f, 0.0f, transform);
	vec3_transform(&handlePos, &handlePos, &data.parent_xform);
	handlePos.x -= rotHandleOffset.x;
	handlePos.y -= rotHandleOffset.y;

	float dist = vec3_dist(&handlePos, &pos3);
	if (dist < data.radius) {
		if (dist < closestHandle) {
			closestHandle = dist;
			data.item = item;
			data.angle = obs_sceneitem_get_rot(item);
			data.handle = ItemHandle::Rot;

			vec2_set(&data.rotatePoint,
				 transform.t.x + transform.x.x / 2 +
					 transform.y.x / 2,
				 transform.t.y + transform.x.y / 2 +
					 transform.y.y / 2);

			obs_sceneitem_get_pos(item, &data.offsetPoint);
			data.offsetPoint.x -= data.rotatePoint.x;
			data.offsetPoint.y -= data.rotatePoint.y;

			RotatePos(&data.offsetPoint,
				  -RAD(obs_sceneitem_get_rot(item)));
		}
	}

	UNUSED_PARAMETER(scene);
	return true;
}

void CanvasDock::GetStretchHandleData(const vec2 &pos, bool ignoreGroup)
{
	if (!scene)
		return;

	float scale = /*main->previewScale / */ GetDevicePixelRatio();
	vec2 scaled_pos = pos;
	vec2_divf(&scaled_pos, &scaled_pos, scale);
	HandleFindData data(scaled_pos, scale);
	obs_scene_enum_items(scene, FindHandleAtPos, &data);

	stretchItem = std::move(data.item);
	stretchHandle = data.handle;

	rotateAngle = data.angle;
	rotatePoint = data.rotatePoint;
	offsetPoint = data.offsetPoint;

	if (stretchHandle != ItemHandle::None) {
		matrix4 boxTransform;
		vec3 itemUL;
		float itemRot;

		stretchItemSize = GetItemSize(stretchItem);

		obs_sceneitem_get_box_transform(stretchItem, &boxTransform);
		itemRot = obs_sceneitem_get_rot(stretchItem);
		vec3_from_vec4(&itemUL, &boxTransform.t);

		/* build the item space conversion matrices */
		matrix4_identity(&itemToScreen);
		matrix4_rotate_aa4f(&itemToScreen, &itemToScreen, 0.0f, 0.0f,
				    1.0f, RAD(itemRot));
		matrix4_translate3f(&itemToScreen, &itemToScreen, itemUL.x,
				    itemUL.y, 0.0f);

		matrix4_identity(&screenToItem);
		matrix4_translate3f(&screenToItem, &screenToItem, -itemUL.x,
				    -itemUL.y, 0.0f);
		matrix4_rotate_aa4f(&screenToItem, &screenToItem, 0.0f, 0.0f,
				    1.0f, RAD(-itemRot));

		obs_sceneitem_get_crop(stretchItem, &startCrop);
		obs_sceneitem_get_pos(stretchItem, &startItemPos);

		obs_source_t *source = obs_sceneitem_get_source(stretchItem);
		cropSize.x = float(obs_source_get_width(source) -
				   startCrop.left - startCrop.right);
		cropSize.y = float(obs_source_get_height(source) -
				   startCrop.top - startCrop.bottom);

		stretchGroup = obs_sceneitem_get_group(scene, stretchItem);
		if (stretchGroup && !ignoreGroup) {
			obs_sceneitem_get_draw_transform(stretchGroup,
							 &invGroupTransform);
			matrix4_inv(&invGroupTransform, &invGroupTransform);
			obs_sceneitem_defer_group_resize_begin(stretchGroup);
		}
	}
}

void CanvasDock::ClampAspect(vec3 &tl, vec3 &br, vec2 &size,
			     const vec2 &baseSize)
{
	float baseAspect = baseSize.x / baseSize.y;
	float aspect = size.x / size.y;
	uint32_t stretchFlags = (uint32_t)stretchHandle;

	if (stretchHandle == ItemHandle::TopLeft ||
	    stretchHandle == ItemHandle::TopRight ||
	    stretchHandle == ItemHandle::BottomLeft ||
	    stretchHandle == ItemHandle::BottomRight) {
		if (aspect < baseAspect) {
			if ((size.y >= 0.0f && size.x >= 0.0f) ||
			    (size.y <= 0.0f && size.x <= 0.0f))
				size.x = size.y * baseAspect;
			else
				size.x = size.y * baseAspect * -1.0f;
		} else {
			if ((size.y >= 0.0f && size.x >= 0.0f) ||
			    (size.y <= 0.0f && size.x <= 0.0f))
				size.y = size.x / baseAspect;
			else
				size.y = size.x / baseAspect * -1.0f;
		}

	} else if (stretchHandle == ItemHandle::TopCenter ||
		   stretchHandle == ItemHandle::BottomCenter) {
		if ((size.y >= 0.0f && size.x >= 0.0f) ||
		    (size.y <= 0.0f && size.x <= 0.0f))
			size.x = size.y * baseAspect;
		else
			size.x = size.y * baseAspect * -1.0f;

	} else if (stretchHandle == ItemHandle::CenterLeft ||
		   stretchHandle == ItemHandle::CenterRight) {
		if ((size.y >= 0.0f && size.x >= 0.0f) ||
		    (size.y <= 0.0f && size.x <= 0.0f))
			size.y = size.x / baseAspect;
		else
			size.y = size.x / baseAspect * -1.0f;
	}

	size.x = std::round(size.x);
	size.y = std::round(size.y);

	if (stretchFlags & ITEM_LEFT)
		tl.x = br.x - size.x;
	else if (stretchFlags & ITEM_RIGHT)
		br.x = tl.x + size.x;

	if (stretchFlags & ITEM_TOP)
		tl.y = br.y - size.y;
	else if (stretchFlags & ITEM_BOTTOM)
		br.y = tl.y + size.y;
}

vec3 CanvasDock::CalculateStretchPos(const vec3 &tl, const vec3 &br)
{
	uint32_t alignment = obs_sceneitem_get_alignment(stretchItem);
	vec3 pos;

	vec3_zero(&pos);

	if (alignment & OBS_ALIGN_LEFT)
		pos.x = tl.x;
	else if (alignment & OBS_ALIGN_RIGHT)
		pos.x = br.x;
	else
		pos.x = (br.x - tl.x) * 0.5f + tl.x;

	if (alignment & OBS_ALIGN_TOP)
		pos.y = tl.y;
	else if (alignment & OBS_ALIGN_BOTTOM)
		pos.y = br.y;
	else
		pos.y = (br.y - tl.y) * 0.5f + tl.y;

	return pos;
}

bool CanvasDock::DrawSelectionBox(float x1, float y1, float x2, float y2,
				  gs_vertbuffer_t *rectFill)
{
	float pixelRatio = GetDevicePixelRatio();

	x1 = std::round(x1);
	x2 = std::round(x2);
	y1 = std::round(y1);
	y2 = std::round(y2);

	gs_effect_t *eff = gs_get_effect();
	gs_eparam_t *colParam = gs_effect_get_param_by_name(eff, "color");

	vec4 fillColor;
	vec4_set(&fillColor, 0.7f, 0.7f, 0.7f, 0.5f);

	vec4 borderColor;
	vec4_set(&borderColor, 1.0f, 1.0f, 1.0f, 1.0f);

	vec2 scale;
	vec2_set(&scale, std::abs(x2 - x1), std::abs(y2 - y1));

	gs_matrix_push();
	gs_matrix_identity();

	gs_matrix_translate3f(x1, y1, 0.0f);
	gs_matrix_scale3f(x2 - x1, y2 - y1, 1.0f);

	gs_effect_set_vec4(colParam, &fillColor);
	gs_load_vertexbuffer(rectFill);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_effect_set_vec4(colParam, &borderColor);
	DrawRect(HANDLE_RADIUS * pixelRatio / 2, scale);

	gs_matrix_pop();

	return true;
}

static bool add_sources_of_type_to_menu(void *param, obs_source_t *source)
{
	QMenu *menu = static_cast<QMenu *>(param);
	CanvasDock *cd = static_cast<CanvasDock *>(menu->parent());
	auto a = menu->menuAction();
	auto t = a->data().toString();
	auto idUtf8 = t.toUtf8();
	const char *id = idUtf8.constData();
	if (strcmp(obs_source_get_unversioned_id(source), id) == 0) {
		menu->addAction(QString::fromUtf8(obs_source_get_name(source)),
				cd,
				[cd, source] { cd->AddSourceToScene(source); });
	}
	return true;
}

void CanvasDock::LoadSourceTypeMenu(QMenu *menu, const char *type)
{
	menu->clear();
	if (strcmp(type, "scene") != 0) {
		auto popupItem = menu->addAction(obs_module_text("New"));
		popupItem->setData(QString::fromUtf8(type));
		connect(popupItem, SIGNAL(triggered(bool)), this,
			SLOT(AddSourceFromAction()));
	}
	menu->addSeparator();
	obs_enum_sources(add_sources_of_type_to_menu, menu);
}

void CanvasDock::AddSourceToScene(obs_source_t *source)
{
	obs_scene_add(scene, source);
}

QMenu *CanvasDock::CreateAddSourcePopupMenu()
{
	const char *unversioned_type;
	const char *type;
	bool foundValues = false;
	bool foundDeprecated = false;
	size_t idx = 0;

	QMenu *popup = new QMenu(obs_module_text("Add"), this);
	QMenu *deprecated = new QMenu(obs_module_text("Deprecated"), popup);

	auto getActionAfter = [](QMenu *menu, const QString &name) {
		QList<QAction *> actions = menu->actions();

		for (QAction *menuAction : actions) {
			if (menuAction->text().compare(name) >= 0)
				return menuAction;
		}

		return (QAction *)nullptr;
	};

	auto addSource = [this, getActionAfter](QMenu *popup, const char *type,
						const char *name) {
		QString qname = QString::fromUtf8(name);
		QAction *popupItem = new QAction(qname, this);
		popupItem->setData(QString::fromUtf8(type));
		QMenu *menu = new QMenu(this);
		popupItem->setMenu(menu);
		QObject::connect(menu, &QMenu::aboutToShow, [this, menu, type] {
			LoadSourceTypeMenu(menu, type);
		});

		/*
		QIcon icon;

		if (strcmp(type, "scene") == 0)
			icon = GetSceneIcon();
		else
			icon = GetSourceIcon(type);

		popupItem->setIcon(icon);*/

		QAction *after = getActionAfter(popup, qname);
		popup->insertAction(after, popupItem);
	};

	while (obs_enum_input_types2(idx++, &type, &unversioned_type)) {
		const char *name = obs_source_get_display_name(type);
		uint32_t caps = obs_get_source_output_flags(type);

		if ((caps & OBS_SOURCE_CAP_DISABLED) != 0)
			continue;

		if ((caps & OBS_SOURCE_DEPRECATED) == 0) {
			addSource(popup, unversioned_type, name);
		} else {
			addSource(deprecated, unversioned_type, name);
			foundDeprecated = true;
		}
		foundValues = true;
	}

	addSource(popup, "scene", obs_module_text("Scene"));

	popup->addSeparator();
	QAction *addGroup = new QAction(obs_module_text("Group"), this);
	addGroup->setData(QString::fromUtf8("group"));
	//addGroup->setIcon(GetGroupIcon());
	connect(addGroup, SIGNAL(triggered(bool)), this,
		SLOT(AddSourceFromAction()));
	popup->addAction(addGroup);

	if (!foundDeprecated) {
		delete deprecated;
		deprecated = nullptr;
	}

	if (!foundValues) {
		delete popup;
		popup = nullptr;

	} else if (foundDeprecated) {
		popup->addSeparator();
		popup->addMenu(deprecated);
	}

	return popup;
}

void CanvasDock::AddSourceFromAction()
{
	QAction *action = qobject_cast<QAction *>(sender());
	if (!action)
		return;

	auto t = action->data().toString();
	auto idUtf8 = t.toUtf8();
	const char *id = idUtf8.constData();
	if (id && *id && strlen(id)) {
		const char *v_id = obs_get_latest_input_type_id(id);
		QString placeHolderText =
			QString::fromUtf8(obs_source_get_display_name(v_id));
		QString text = placeHolderText;
		int i = 2;
		OBSSourceAutoRelease s = nullptr;
		while ((s = obs_get_source_by_name(text.toUtf8().constData()))) {
			text = QString("%1 %2").arg(placeHolderText).arg(i++);
		}
		obs_source_t *source = obs_source_create(
			id, text.toUtf8().constData(), nullptr, nullptr);
		obs_scene_add(scene, source);
		if (obs_source_configurable(source)) {
			obs_frontend_open_source_properties(source);
		}
		obs_source_release(source);
	}
}

bool CanvasDock::StartVideo()
{
	if (!view)
		view = obs_view_create();

	auto s = obs_weak_source_get_source(source);

	obs_view_set_source(view, 0, s);
	bool started_video = false;
	if (!video) {
		obs_video_info ovi;
		obs_get_video_info(&ovi);
		ovi.base_width = obs_source_get_width(s);
		ovi.base_height = obs_source_get_height(s);
		ovi.output_width = ovi.base_width;
		ovi.output_height = ovi.base_height;
		video = obs_view_add2(view, &ovi);
		started_video = true;
	}
	obs_source_release(s);
	return started_video;
}

void CanvasDock::virtual_cam_ouput_start(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	auto d = static_cast<CanvasDock *>(data);
	QMetaObject::invokeMethod(d, "OnVirtualCamStart");
}

void CanvasDock::virtual_cam_ouput_stop(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	auto d = static_cast<CanvasDock *>(data);
	QMetaObject::invokeMethod(d, "OnVirtualCamStop");
	if (d->video) {
		obs_view_remove(d->view);
		obs_view_set_source(d->view, 0, nullptr);
		d->video = nullptr;
	}
	if (d->virtualCamOutput) {
		obs_output_release(d->virtualCamOutput);
		d->virtualCamOutput = nullptr;
	}
}

void CanvasDock::OnVirtualCamStart()
{
	virtualCamButton->setChecked(true);
}

void CanvasDock::OnVirtualCamStop()
{
	virtualCamButton->setChecked(false);
}

void CanvasDock::VirtualCamButtonClicked()
{
	if (virtualCamOutput) {
		StopVirtualCam();
	} else {
		StartVirtualCam();
	}
}

void CanvasDock::StartVirtualCam()
{
	const auto output = obs_frontend_get_virtualcam_output();
	if (obs_output_active(output))
		return;

	virtualCamOutput = output;

	bool started_video = StartVideo();
	signal_handler_t *signal = obs_output_get_signal_handler(output);
	signal_handler_disconnect(signal, "start", virtual_cam_ouput_start,
				  this);
	signal_handler_disconnect(signal, "stop", virtual_cam_ouput_stop, this);
	signal_handler_connect(signal, "start", virtual_cam_ouput_start, this);
	signal_handler_connect(signal, "stop", virtual_cam_ouput_stop, this);

	obs_output_set_media(output, video, obs_get_audio());

	bool success = obs_output_start(output);
	if (!success && started_video) {
		obs_view_remove(view);
		obs_view_set_source(view, 0, nullptr);
		video = nullptr;
	}
}

void CanvasDock::StopVirtualCam()
{
	if (!virtualCamOutput || !obs_output_active(virtualCamOutput)) {
		virtualCamButton->setChecked(false);
		return;
	}
	obs_output_set_media(virtualCamOutput, nullptr, nullptr);
	obs_output_stop(virtualCamOutput);
}

void CanvasDock::ConfigButtonClicked()
{
	if (!configDialog)
		configDialog = new MultiCanvasConfigDialog(
			(QMainWindow *)obs_frontend_get_main_window());
	auto result = configDialog->exec();
	if (result == 1) {
	}
}

void CanvasDock::ReplayButtonClicked()
{
	//replayOutput = obs_output_create("replay_buffer")
}

int GetConfigPath(char *path, size_t size, const char *name)
{
#if ALLOW_PORTABLE_MODE
	if (portable_mode) {
		if (name && *name) {
			return snprintf(path, size, CONFIG_PATH "/%s", name);
		} else {
			return snprintf(path, size, CONFIG_PATH);
		}
	} else {
		return os_get_config_path(path, size, name);
	}
#else
	return os_get_config_path(path, size, name);
#endif
}

static inline int GetProfilePath(char *path, size_t size, const char *file)
{
	char profiles_path[512];
	const char *profile = config_get_string(
		obs_frontend_get_global_config(), "Basic", "ProfileDir");
	int ret;

	if (!profile)
		return -1;
	if (!path)
		return -1;
	if (!file)
		file = "";

	ret = GetConfigPath(profiles_path, 512, "obs-studio/basic/profiles");
	if (ret <= 0)
		return ret;

	if (!*file)
		return snprintf(path, size, "%s/%s", profiles_path, profile);

	return snprintf(path, size, "%s/%s/%s", profiles_path, profile, file);
}

static OBSData GetDataFromJsonFile(const char *jsonFile)
{
	char fullPath[512];
	OBSDataAutoRelease data = nullptr;

	int ret = GetProfilePath(fullPath, sizeof(fullPath), jsonFile);
	if (ret > 0) {
		BPtr<char> jsonData = os_quick_read_utf8_file(fullPath);
		if (!!jsonData) {
			data = obs_data_create_from_json(jsonData);
		}
	}

	if (!data)
		data = obs_data_create();

	return data.Get();
}

static void ensure_directory(char *path)
{
#ifdef _WIN32
	char *backslash = strrchr(path, '\\');
	if (backslash)
		*backslash = '/';
#endif

	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = 0;
		os_mkdirs(path);
		*slash = '/';
	}

#ifdef _WIN32
	if (backslash)
		*backslash = '\\';
#endif
}

void CanvasDock::RecordButtonClicked()
{
	if (recordOutput) {
		StopRecord();
	} else {
		StartRecord();
	}
}

void CanvasDock::StartRecord()
{
	if (recordOutput)
		return;

	obs_output_t *replay_output = obs_frontend_get_replay_buffer_output();
	if (replay_output) {
		obs_encoder_t *ve = obs_output_get_video_encoder(replay_output);
		if (!ve) {
			obs_frontend_replay_buffer_start();
			obs_frontend_replay_buffer_stop();
		}
		obs_output_release(replay_output);
	}
	obs_output_t *output = obs_frontend_get_recording_output();
	recordOutput = obs_output_create(obs_output_get_id(output),
					 "multi_canvas_record", nullptr,
					 nullptr);

	obs_output_set_mixers(recordOutput, obs_output_get_mixers(output));
	obs_data_t *settings = obs_output_get_settings(output);
	obs_output_update(recordOutput, settings);
	obs_data_release(settings);

	obs_encoder_t *enc = obs_output_get_video_encoder(output);
	obs_encoder_t *video_encoder = obs_video_encoder_create(
		obs_encoder_get_id(enc), "multi_canvas_record_video_encoder",
		nullptr, nullptr);
	obs_data_t *d = obs_encoder_get_settings(video_encoder);
	obs_encoder_update(video_encoder, d);
	obs_data_release(d);
	obs_encoder_release(enc);

	obs_output_set_video_encoder(recordOutput, video_encoder);
	//obs_encoder_release(video_encoder);

	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		obs_encoder_t *audio_encoder =
			obs_output_get_audio_encoder(output, i);
		obs_output_set_audio_encoder(recordOutput, audio_encoder, i);
		obs_encoder_release(audio_encoder);
	}

	const bool started_video = StartVideo();

	signal_handler_t *signal = obs_output_get_signal_handler(recordOutput);
	signal_handler_disconnect(signal, "start", record_ouput_start, this);
	signal_handler_disconnect(signal, "stop", record_ouput_stop, this);
	signal_handler_disconnect(signal, "stopping", record_ouput_stopping,
				  this);
	signal_handler_connect(signal, "start", record_ouput_start, this);
	signal_handler_connect(signal, "stop", record_ouput_stop, this);
	signal_handler_connect(signal, "stopping", record_ouput_stopping, this);

	config_t *config = obs_frontend_get_profile_config();
	const char *mode = config_get_string(config, "Output", "Mode");
	const char *dir = nullptr;
	const char *format = nullptr;
	bool ffmpegOutput = false;
	if (strcmp(mode, "Advanced") == 0) {
		const char *recType =
			config_get_string(config, "AdvOut", "RecType");

		if (strcmp(recType, "FFmpeg") == 0) {
			ffmpegOutput = true;
			dir = config_get_string(config, "AdvOut", "FFFilePath");
		} else {
			dir = config_get_string(config, "AdvOut",
						"RecFilePath");
		}
		bool ffmpegRecording =
			ffmpegOutput &&
			config_get_bool(config, "AdvOut", "FFOutputToFile");
		format = config_get_string(config, "AdvOut",
					   ffmpegRecording ? "FFExtension"
							   : "RecFormat");
	} else {
		dir = config_get_string(config, "SimpleOutput", "FilePath");
		format = config_get_string(config, "SimpleOutput", "RecFormat");
		const char *quality =
			config_get_string(config, "SimpleOutput", "RecQuality");
		if (strcmp(quality, "Lossless") == 0) {
			ffmpegOutput = true;
		}
	}
	const char *filenameFormat =
		config_get_string(config, "Output", "FilenameFormatting");

	obs_data_t *ps = obs_data_create();
	char path[512];
	char *filename = os_generate_formatted_filename(
		ffmpegOutput ? "avi" : format, true, filenameFormat);
	snprintf(path, 512, "%s/%s", dir, filename);
	bfree(filename);
	ensure_directory(path);
	obs_data_set_string(ps, ffmpegOutput ? "url" : "path", path);
	obs_output_update(recordOutput, ps);
	obs_data_release(ps);

	obs_encoder_set_video(video_encoder, video);
	obs_output_set_media(recordOutput, video, obs_get_audio());

	const bool success = obs_output_start(recordOutput);
	if (!success && started_video) {
		const char *error = obs_output_get_last_error(recordOutput);
		QString error_reason = QString::fromUtf8(
			error ? error
			      : obs_module_text("Output.StartFailedGeneric"));
		obs_view_remove(view);
		obs_view_set_source(view, 0, nullptr);
		video = nullptr;
	}
}

void CanvasDock::StopRecord()
{
	if (!recordOutput || !obs_output_active(recordOutput)) {
		recordButton->setChecked(false);
		return;
	}
	obs_output_stop(recordOutput);
}

void CanvasDock::record_ouput_start(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	auto d = static_cast<CanvasDock *>(data);
	QMetaObject::invokeMethod(d, "OnRecordStart");
}

void CanvasDock::record_ouput_stop(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	auto d = static_cast<CanvasDock *>(data);
	QMetaObject::invokeMethod(d, "OnRecordStop");
	if (d->video) {
		obs_view_remove(d->view);
		obs_view_set_source(d->view, 0, nullptr);
		d->video = nullptr;
	}
	if (d->recordOutput) {
		obs_output_release(d->recordOutput);
		d->recordOutput = nullptr;
	}
}

void CanvasDock::record_ouput_stopping(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	UNUSED_PARAMETER(data);
	//auto d = static_cast<CanvasDock *>(data);
}

void CanvasDock::StreamButtonClicked() {}
