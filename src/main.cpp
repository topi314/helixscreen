// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file main.cpp
 * @brief Application entry point
 *
 * This file is intentionally minimal. All application logic is implemented
 * in the Application class (src/application/application.cpp).
 *
 * @see Application
 */

#include "app_globals.h"
#include "application.h"
#include "async_lifetime_guard.h"
#include "data_root_resolver.h"
#include "helix_version.h"
#include "system/crash_handler.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <unistd.h>

// SDL2 redefines main → SDL_main via this header.
// On Android, the SDL Java activity loads libmain.so and calls SDL_main().
// Without this include, the symbol is missing and the app crashes on launch.
#ifdef HELIX_PLATFORM_ANDROID
#include <SDL.h>
#endif

// Log to stderr using only async-signal-safe-ish functions.
// spdlog may not be initialized yet or may be in a broken state.
static void log_fatal(const char* msg) {
    fprintf(stderr, "[FATAL] %s\n", msg);
    fflush(stderr);
}

// Called by std::terminate() — covers uncaught exceptions, joinable thread
// destruction, and other fatal C++ runtime errors. Logs what we can before
// the default terminate handler calls abort() (which triggers crash_handler).
static void terminate_handler() {
    // Guard against re-entrance (e.g. exception::what() throws). The reason — if
    // already determined — was stashed via crash_handler::set_terminate_context()
    // below, so this bare abort() still surfaces it as `terminate_msg:` in the
    // SIGABRT record instead of producing a blank crash (issue #987).
    static bool entered = false;
    if (entered) {
        abort();
    }
    entered = true;

    // Stash a placeholder reason up front so even a fault while inspecting the
    // exception (rethrow / what()) leaves a trace — refined with the real reason
    // once it's known below.
    crash_handler::set_terminate_context("std::terminate (reason capture in progress)");

    // Check if there's a current exception we can inspect
    const char* what = nullptr;
    if (auto eptr = std::current_exception()) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            what = e.what();
            fprintf(stderr, "[FATAL] Uncaught exception: %s\n", what);
            fflush(stderr);
        } catch (...) {
            log_fatal("Uncaught non-std::exception");
            what = "non-std::exception";
        }
    } else {
        log_fatal("std::terminate() called without active exception "
                  "(joinable thread destroyed? noexcept violation?)");
        what = "std::terminate without active exception";
    }

    // Refine the stashed reason now that the real exception text is known, so a
    // re-fault inside write_exception_record() still reports it (issue #987).
    crash_handler::set_terminate_context(what);

    // Write crash file BEFORE abort — abort triggers the signal handler which
    // would overwrite it without the exception message.
    crash_handler::write_exception_record(what);

    // Encode signal death via POSIX 128+signum convention so the watchdog's
    // exit-code translation (helix_watchdog.cpp: "exited with code N (signal N
    // via crash handler)") classifies this as a crash and shows the recovery
    // dialog. _exit(1) used to be here but the watchdog treated it as a clean
    // non-zero exit ("not a crash") and restarted silently — crash.txt was
    // written but the user never saw the dialog.
    _exit(128 + SIGABRT);
}

int main(int argc, char** argv) {
    // Record the main thread id before any thread that uses LifetimeToken
    // can spawn. The bg-thread expired() detector compares against this.
    helix::internal::set_main_thread_id();

    std::set_terminate(terminate_handler);

    int rc = 1;
    try {
        Application app;
        rc = app.run(argc, argv);
    } catch (const std::exception& e) {
        fprintf(stderr, "[FATAL] Unhandled exception in Application: %s\n", e.what());
        fflush(stderr);
        crash_handler::write_exception_record(e.what());
        _exit(128 + SIGABRT);
    } catch (...) {
        log_fatal("Unhandled non-std::exception in Application");
        crash_handler::write_exception_record("non-std::exception");
        _exit(128 + SIGABRT);
    }

    // In-place restart: if the UI asked for a restart, replace this process
    // image with a fresh instance.  Cleanup ran on the way out of app.run(),
    // so the lockfile is released and LVGL/display state is torn down — the
    // new instance comes up clean.  See app_globals.cpp app_request_restart().
    if (app_restart_after_quit_requested()) {
        char** new_argv = app_get_stored_argv();
        const char* exe = app_get_executable_path();
        if (exe && new_argv) {
            fprintf(stderr, "[App] In-place restart: execv(%s)\n", exe);
            fflush(stderr);
            execv(exe, new_argv);
            fprintf(stderr, "[App] execv failed: %s\n", strerror(errno));
            fflush(stderr);
            return 1;
        }
        fprintf(stderr, "[App] Restart requested but argv/exe unavailable\n");
        fflush(stderr);
    }
    return rc;
}
