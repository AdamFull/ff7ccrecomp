package com.ff7ccrecomp;

import org.libsdl.app.SDLActivity;

/** Hosts the game: SDL loads libmain.so (SDL3 is linked statically into it) and calls SDL_main. */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }
}
