package com.gowa.app;

import org.libsdl.app.SDLActivity;

/** The whole Activity: SDL3 owns the window/surface and the lifecycle callbacks; the game runs in libgowa_app.so (SDL_main). */
public class GowActivity extends SDLActivity {
    @Override protected String[] getLibraries() { return new String[] { "SDL3", "gowa_app" }; }
}
