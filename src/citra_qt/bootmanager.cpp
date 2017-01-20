#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>

#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
// Required for screen DPI information
#include <QScreen>
#include <QWindow>
#endif

#include "citra_qt/bootmanager.h"
#include "common/microprofile.h"
#include "common/scm_rev.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/frontend/key_map.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/video_core.h"

// Required for SDL gamepad support
#include <SDL.h>
#include <SDL_events.h>

EmuThread::EmuThread(GRenderWindow* render_window)
    : exec_step(false), running(false), stop_run(false), render_window(render_window) {}

void EmuThread::run() {
    render_window->MakeCurrent();

    MicroProfileOnThreadCreate("EmuThread");

    stop_run = false;

    // holds whether the cpu was running during the last iteration,
    // so that the DebugModeLeft signal can be emitted before the
    // next execution step
    bool was_active = false;
    while (!stop_run) {
        if (running) {
            if (!was_active)
                emit DebugModeLeft();

            Core::System::GetInstance().RunLoop();

            was_active = running || exec_step;
            if (!was_active && !stop_run)
                emit DebugModeEntered();
        } else if (exec_step) {
            if (!was_active)
                emit DebugModeLeft();

            exec_step = false;
            Core::System::GetInstance().SingleStep();
            emit DebugModeEntered();
            yieldCurrentThread();

            was_active = false;
        } else {
            std::unique_lock<std::mutex> lock(running_mutex);
            running_cv.wait(lock, [this] { return IsRunning() || exec_step || stop_run; });
        }
    }

    // Shutdown the core emulation
    Core::System::GetInstance().Shutdown();

#if MICROPROFILE_ENABLED
    MicroProfileOnThreadExit();
#endif

    render_window->moveContext();
}

// This class overrides paintEvent and resizeEvent to prevent the GUI thread from stealing GL
// context.
// The corresponding functionality is handled in EmuThread instead
class GGLWidgetInternal : public QGLWidget {
public:
    GGLWidgetInternal(QGLFormat fmt, GRenderWindow* parent)
        : QGLWidget(fmt, parent), parent(parent) {}

    void paintEvent(QPaintEvent* ev) override {
        if (do_painting) {
            QPainter painter(this);
        }
    }

    void resizeEvent(QResizeEvent* ev) override {
        parent->OnClientAreaResized(ev->size().width(), ev->size().height());
        parent->OnFramebufferSizeChanged();
    }

    void DisablePainting() {
        do_painting = false;
    }
    void EnablePainting() {
        do_painting = true;
    }

private:
    GRenderWindow* parent;
    bool do_painting;
};

GRenderWindow::GRenderWindow(QWidget* parent, EmuThread* emu_thread)
    : QWidget(parent), child(nullptr), keyboard_id(0), emu_thread(emu_thread) {

    std::string window_title =
        Common::StringFromFormat("Citra | %s-%s", Common::g_scm_branch, Common::g_scm_desc);
    setWindowTitle(QString::fromStdString(window_title));

    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0) {
        LOG_CRITICAL(Frontend, "Failed to initialize SDL2 gamepad! Exiting...");
        exit(1);
    }

    GamepadSetMappings();


    keyboard_id = KeyMap::NewDeviceId();
    ReloadSetKeymaps();
}

void GRenderWindow::moveContext() {
    DoneCurrent();
// We need to move GL context to the swapping thread in Qt5
#if QT_VERSION > QT_VERSION_CHECK(5, 0, 0)
    // If the thread started running, move the GL Context to the new thread. Otherwise, move it
    // back.
    auto thread = (QThread::currentThread() == qApp->thread() && emu_thread != nullptr)
                      ? emu_thread
                      : qApp->thread();
    child->context()->moveToThread(thread);
#endif
}

void GRenderWindow::SwapBuffers() {
#if !defined(QT_NO_DEBUG)
    // Qt debug runtime prints a bogus warning on the console if you haven't called makeCurrent
    // since the last time you called swapBuffers. This presumably means something if you're using
    // QGLWidget the "regular" way, but in our multi-threaded use case is harmless since we never
    // call doneCurrent in this thread.
    child->makeCurrent();
#endif
    child->swapBuffers();
}

void GRenderWindow::MakeCurrent() {
    child->makeCurrent();
}

void GRenderWindow::DoneCurrent() {
    child->doneCurrent();
}

void GRenderWindow::PollEvents() {
    // Poll for gamepad controller button events and axis motion, maybe this should be changed into 2 calls to SDL_AddEventWatch?
    // Might be more congruent with the rest of the code (i.e., only having one SDL event loop)
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERBUTTONDOWN:
            gamepadButtonEvent(&event.cbutton);
            break;
        case SDL_CONTROLLERAXISMOTION:
            gamepadAxisEvent(&event.caxis);
            break;
        }
    }
}

void GRenderWindow::GamepadSetMappings() {
    SDL_GameControllerAddMapping("78696e70757401000000000000000000, XInput Controller, a:b0, b : b1, back : b6, dpdown : h0.4, dpleft : h0.8, dpright : h0.2, dpup : h0.1, guide : b10, leftshoulder : b4, leftstick : b8, lefttrigger : a2, leftx : a0, lefty : a1, rightshoulder : b5, rightstick : b9, righttrigger : a5, rightx : a3, righty : a4, start : b7, x : b2, y : b3, ");

    gamepad_controllers = std::vector<SDL_GameController*>();
    gamepad_mappings = std::vector<std::tuple<SDL_GameControllerButton, int>>();

    gamepad_mappings.push_back(std::tuple<SDL_GameControllerButton, int>(SDL_CONTROLLER_BUTTON_A, 65));
    gamepad_mappings.push_back(std::tuple<SDL_GameControllerButton, int>(SDL_CONTROLLER_BUTTON_B, 83));
    gamepad_mappings.push_back(std::tuple<SDL_GameControllerButton, int>(SDL_CONTROLLER_BUTTON_X, 90));
    gamepad_mappings.push_back(std::tuple<SDL_GameControllerButton, int>(SDL_CONTROLLER_BUTTON_Y, 88));

    gamepad_mappings.push_back(std::tuple<SDL_GameControllerButton, int>(SDL_CONTROLLER_BUTTON_START, 77));
    gamepad_mappings.push_back(std::tuple<SDL_GameControllerButton, int>(SDL_CONTROLLER_BUTTON_BACK, 78));

    gamepad_mappings.push_back(std::tuple<SDL_GameControllerButton, int>(SDL_CONTROLLER_BUTTON_DPAD_DOWN, 71));
    gamepad_mappings.push_back(std::tuple<SDL_GameControllerButton, int>(SDL_CONTROLLER_BUTTON_DPAD_LEFT, 70));
    gamepad_mappings.push_back(std::tuple<SDL_GameControllerButton, int>(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 72));
    gamepad_mappings.push_back(std::tuple<SDL_GameControllerButton, int>(SDL_CONTROLLER_BUTTON_DPAD_UP, 84));

    int MaxJoysticks = SDL_NumJoysticks() + 1;
    int ControllerIndex = 0;
    for (int JoystickIndex = 0; JoystickIndex < MaxJoysticks; ++JoystickIndex)
    {
        SDL_GameController* pad = SDL_GameControllerOpen(JoystickIndex);
        SDL_Joystick* stick = SDL_JoystickOpen(JoystickIndex);

        gamepad_controllers.push_back(pad);
    }
}

// On Qt 5.0+, this correctly gets the size of the framebuffer (pixels).
//
// Older versions get the window size (density independent pixels),
// and hence, do not support DPI scaling ("retina" displays).
// The result will be a viewport that is smaller than the extent of the window.
void GRenderWindow::OnFramebufferSizeChanged() {
    // Screen changes potentially incur a change in screen DPI, hence we should update the
    // framebuffer size
    qreal pixelRatio = windowPixelRatio();
    unsigned width = child->QPaintDevice::width() * pixelRatio;
    unsigned height = child->QPaintDevice::height() * pixelRatio;
    UpdateCurrentFramebufferLayout(width, height);
}

void GRenderWindow::BackupGeometry() {
    geometry = ((QGLWidget*)this)->saveGeometry();
}

void GRenderWindow::RestoreGeometry() {
    // We don't want to back up the geometry here (obviously)
    QWidget::restoreGeometry(geometry);
}

void GRenderWindow::restoreGeometry(const QByteArray& geometry) {
    // Make sure users of this class don't need to deal with backing up the geometry themselves
    QWidget::restoreGeometry(geometry);
    BackupGeometry();
}

QByteArray GRenderWindow::saveGeometry() {
    // If we are a top-level widget, store the current geometry
    // otherwise, store the last backup
    if (parent() == nullptr)
        return ((QGLWidget*)this)->saveGeometry();
    else
        return geometry;
}

qreal GRenderWindow::windowPixelRatio() {
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    // windowHandle() might not be accessible until the window is displayed to screen.
    return windowHandle() ? windowHandle()->screen()->devicePixelRatio() : 1.0f;
#else
    return 1.0f;
#endif
}

void GRenderWindow::closeEvent(QCloseEvent* event) {
    motion_emu = nullptr;
    emit Closed();
    QWidget::closeEvent(event);
}

void GRenderWindow::keyPressEvent(QKeyEvent* event) {
    KeyMap::PressKey(*this, {event->key(), keyboard_id});
}

void GRenderWindow::keyReleaseEvent(QKeyEvent* event) {
    KeyMap::ReleaseKey(*this, {event->key(), keyboard_id});
}

void GRenderWindow::mousePressEvent(QMouseEvent* event) {
    auto pos = event->pos();
    if (event->button() == Qt::LeftButton) {
        qreal pixelRatio = windowPixelRatio();
        this->TouchPressed(static_cast<unsigned>(pos.x() * pixelRatio),
                           static_cast<unsigned>(pos.y() * pixelRatio));
    } else if (event->button() == Qt::RightButton) {
        motion_emu->BeginTilt(pos.x(), pos.y());
    }
}

void GRenderWindow::mouseMoveEvent(QMouseEvent* event) {
    auto pos = event->pos();
    qreal pixelRatio = windowPixelRatio();
    this->TouchMoved(std::max(static_cast<unsigned>(pos.x() * pixelRatio), 0u),
                     std::max(static_cast<unsigned>(pos.y() * pixelRatio), 0u));
    motion_emu->Tilt(pos.x(), pos.y());
}

void GRenderWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        this->TouchReleased();
    else if (event->button() == Qt::RightButton)
        motion_emu->EndTilt();
}

void GRenderWindow::gamepadButtonEvent(SDL_ControllerButtonEvent* event) {
    auto it = std::find_if(gamepad_mappings.begin(), gamepad_mappings.end(), [event](const std::tuple<SDL_GameControllerButton, int>& e) {return std::get<0>(e) == event->button; });

    if (it != gamepad_mappings.end()) {
        int keycode = std::get<1>(*it);

        if (event->state == SDL_PRESSED) {
            KeyMap::PressKey(*this, { keycode, keyboard_id });
        } else {
            KeyMap::ReleaseKey(*this, { keycode, keyboard_id });
        }
    }
}

void GRenderWindow::gamepadAxisEvent(SDL_ControllerAxisEvent* event) {
    if (event->axis == SDL_CONTROLLER_AXIS_LEFTX) {
        if (event->value < -8000) {
            KeyMap::PressKey(*this, { 16777234, keyboard_id });
        } else {
            KeyMap::ReleaseKey(*this, { 16777234, keyboard_id });
        }

        if (event->value > 8000) {
            KeyMap::PressKey(*this, { 16777236, keyboard_id });
        } else {
            KeyMap::ReleaseKey(*this, { 16777236, keyboard_id });
        }
    }

    if (event->axis == SDL_CONTROLLER_AXIS_LEFTY) {
        if (event->value < -8000) { 
            KeyMap::PressKey(*this, { 16777235, keyboard_id });
        } else {
            KeyMap::ReleaseKey(*this, { 16777235, keyboard_id });
        }

        if (event->value > 8000) {
            KeyMap::PressKey(*this, { 16777237, keyboard_id });
        } else {
            KeyMap::ReleaseKey(*this, { 16777237, keyboard_id });
        }
    }
}


void GRenderWindow::ReloadSetKeymaps() {
    KeyMap::ClearKeyMapping(keyboard_id);
    for (int i = 0; i < Settings::NativeInput::NUM_INPUTS; ++i) {
        KeyMap::SetKeyMapping(
            {Settings::values.input_mappings[Settings::NativeInput::All[i]], keyboard_id},
            KeyMap::mapping_targets[i]);
    }
}

void GRenderWindow::OnClientAreaResized(unsigned width, unsigned height) {
    NotifyClientAreaSizeChanged(std::make_pair(width, height));
}

void GRenderWindow::InitRenderTarget() {
    if (child) {
        delete child;
    }

    if (layout()) {
        delete layout();
    }

    // TODO: One of these flags might be interesting: WA_OpaquePaintEvent, WA_NoBackground,
    // WA_DontShowOnScreen, WA_DeleteOnClose
    QGLFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QGLFormat::CoreProfile);
    fmt.setSwapInterval(Settings::values.use_vsync);

    // Requests a forward-compatible context, which is required to get a 3.2+ context on OS X
    fmt.setOption(QGL::NoDeprecatedFunctions);

    child = new GGLWidgetInternal(fmt, this);
    QBoxLayout* layout = new QHBoxLayout(this);

    resize(VideoCore::kScreenTopWidth,
           VideoCore::kScreenTopHeight + VideoCore::kScreenBottomHeight);
    layout->addWidget(child);
    layout->setMargin(0);
    setLayout(layout);

    OnMinimalClientAreaChangeRequest(GetActiveConfig().min_client_area_size);

    OnFramebufferSizeChanged();
    NotifyClientAreaSizeChanged(std::pair<unsigned, unsigned>(child->width(), child->height()));

    BackupGeometry();
}

void GRenderWindow::OnMinimalClientAreaChangeRequest(
    const std::pair<unsigned, unsigned>& minimal_size) {
    setMinimumSize(minimal_size.first, minimal_size.second);
}

void GRenderWindow::OnEmulationStarting(EmuThread* emu_thread) {
    motion_emu = std::make_unique<Motion::MotionEmu>(*this);
    this->emu_thread = emu_thread;
    child->DisablePainting();
}

void GRenderWindow::OnEmulationStopping() {
    motion_emu = nullptr;
    emu_thread = nullptr;
    child->EnablePainting();
}

void GRenderWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);

#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    // windowHandle() is not initialized until the Window is shown, so we connect it here.
    connect(this->windowHandle(), SIGNAL(screenChanged(QScreen*)), this,
            SLOT(OnFramebufferSizeChanged()), Qt::UniqueConnection);
#endif
}
