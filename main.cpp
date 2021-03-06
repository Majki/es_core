/*
Copyright (c) 2013, Timothee Besset
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Timothee Besset nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL Timothee Besset BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "SDL.h"
#include "SDL_opengl.h"
#include "SDL_sysvideo.h"
#include "SDL_thread.h"
// bad SDL, bad.. seems to be Windows only at least
#ifdef main
#undef main
#endif

#include "OgreRoot.h"
#include "OgreRenderWindow.h"

#include "czmq.h"

#ifdef __WINDOWS__
  #include "SDL_windowswindow.h"
#elif __LINUX__
  #include "SDL_x11video.h"
#elif __APPLE__
  #include "OSX_wrap.h"
#endif

#include "render.h"
#include "game.h"

typedef struct InputState_s {
  float yaw_sens;
  float pitch_sens;
  float yaw;			// degrees, modulo ]-180,180] range
  float pitch;			// degrees, clamped [-90,90] range
  float roll;
  Ogre::Quaternion orientation;	// current orientation
} InputState;

void parse_orientation( char * start, Ogre::Quaternion & orientation ) {
  char * end = strchr( start, ' ' );
  end[0] = '\0';
  orientation.w = atof( start );
  start = end + 1;
  end = strchr( start, ' ' );
  end[0] = '\0';
  orientation.x = atof( start );
  start = end + 1;
  end = strchr( start, ' ' );
  end[0] = '\0';
  orientation.y = atof( start );
  start = end + 1;
  orientation.z = atof( start );
}

void send_shutdown( void * zmq_render_socket, void * zmq_game_socket ) {
  zstr_send( zmq_render_socket, "stop" );
  zstr_send( zmq_game_socket, "stop" );  
}

void wait_shutdown( SDL_Thread * & sdl_render_thread, SDL_Thread * & sdl_game_thread ) {
  {
    int status;
    SDL_WaitThread( sdl_render_thread, &status );
    printf( "render thread has stopped, status: %d\n", status );
    sdl_render_thread = NULL;
  }
  {
    int status;
    SDL_WaitThread( sdl_game_thread, &status );
    printf( "game thread has stopped, status: %d\n", status );
    sdl_game_thread = NULL;
  }
}

//int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int windowStyle ) {
int main( int argc, char *argv[] ) {
  int retcode = 0;

#ifdef __APPLE__
  // set the working directory to the top directory of the bundle
  struct stat st;
  if ( stat( "media", &st ) != 0 ) {
    //    printf( "%s\n", argv[0] );    
    syslog( LOG_ALERT, "%s\n", argv[0] );
    char bindir[PATH_MAX];
    strcpy( bindir, argv[0] );
    char * stop = strrchr( bindir, '/' );
    *stop = '\0';
    char cwd[PATH_MAX];
    sprintf( cwd, "%s/../../..", bindir );
    //    printf( "chdir %s\n", buf );
    syslog( LOG_ALERT, "chdir %s\n", cwd );
    chdir( cwd );
    assert( stat( "media", &st ) == 0 );
  }  
#endif

  SDL_Init( SDL_INIT_EVERYTHING );
  SDL_Window * window = SDL_CreateWindow(
					 "es_core::SDL",
					 SDL_WINDOWPOS_UNDEFINED,
					 SDL_WINDOWPOS_UNDEFINED,
					 800,
					 600,
					 SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN/*|SDL_WINDOW_RESIZABLE*/ );
  if ( window == NULL ) {
    printf( "SDL_CreateWindow failed: %s\n", SDL_GetError() );
    return -1;
  }

  SDL_GLContext glcontext = NULL;
#ifndef __APPLE__
  glcontext = SDL_GL_CreateContext( window );
  if ( glcontext == NULL ) {
    printf( "SDL_GL_CreateContext failed: %s\n", SDL_GetError() );
    return -1;
  }
#endif
  
  try {

    Ogre::Root * ogre_root = new Ogre::Root();
#ifdef __WINDOWS__
#ifdef _DEBUG
    ogre_root->loadPlugin( "RenderSystem_GL_d" );
#else
    ogre_root->loadPlugin( "RenderSystem_GL" );
#endif
#else
    ogre_root->loadPlugin( "RenderSystem_GL" );
#endif
    if ( ogre_root->getAvailableRenderers().size() != 1 ) {
      OGRE_EXCEPT( Ogre::Exception::ERR_INTERNAL_ERROR, "Failed to initialize RenderSystem_GL", "main" );
    }
    ogre_root->setRenderSystem( ogre_root->getAvailableRenderers()[0] );
    ogre_root->initialise( false );

    Ogre::NameValuePairList params;
#ifdef __WINDOWS__
    params["externalGLControl"] = "1";
    // only supported for Win32 on Ogre 1.8 not on other platforms (documentation needs fixing to accurately reflect this)
    params["externalGLContext"] = Ogre::StringConverter::toString( (unsigned long)glcontext );
    SDL_WindowData * win32_window_data = (SDL_WindowData*)window->driverdata;
    params["externalWindowHandle"] = Ogre::StringConverter::toString( (unsigned long)win32_window_data->hwnd );
#elif __LINUX__
    params["externalGLControl"] = "1";
    // not documented in Ogre 1.8 mainline, supported for GLX and EGL{Win32,X11}
    params["currentGLContext"] = "1";
    // NOTE: externalWindowHandle is reported as deprecated (GLX Ogre 1.8)
    SDL_WindowData * x11_window_data = (SDL_WindowData*)window->driverdata;
    params["parentWindowHandle"] = Ogre::StringConverter::toString( x11_window_data->xwindow );
#elif __APPLE__
    params["externalGLControl"] = "1";
    // only supported for Win32 on Ogre 1.8 not on other platforms (documentation needs fixing to accurately reflect this)
    //    params["externalGLContext"] = Ogre::StringConverter::toString( glcontext );
    params["externalWindowHandle"] = OSX_cocoa_view( window );
    params["macAPI"] = "cocoa";
    params["macAPICocoaUseNSView"] = "true";
#endif
    Ogre::RenderWindow * ogre_render_window = ogre_root->createRenderWindow( "Legion::core::ogre", 800, 600, false, &params );
#ifdef __APPLE__
    // I suspect triple buffering is on by default, which makes vsync pointless?
    // except maybe for poorly implemented render loops which will then be forced to wait
    //    ogre_render_window->setVSyncEnabled( false );
#else
    // NOTE: SDL_GL_SWAP_CONTROL was SDL 1.2 and has been retired
    SDL_GL_SetSwapInterval( 1 );
#endif

    // NOTE: since we are driving with SDL, we need to keep the Ogre side updated for window visibility
    ogre_render_window->setVisible( true );

    zctx_t * zmq_context = zctx_new();

    void * zmq_game_socket = zsocket_new( zmq_context, ZMQ_PAIR );
    zsocket_bind( zmq_game_socket, "inproc://control_game" );
    
    GameThreadParms game_thread_parms;
    game_thread_parms.zmq_context = zmq_context;
    SDL_Thread * sdl_game_thread = SDL_CreateThread( game_thread, "game", &game_thread_parms );

    void * zmq_render_socket = zsocket_new( zmq_context, ZMQ_PAIR );
    zsocket_bind( zmq_render_socket, "inproc://control_render" );

    void * zmq_input_rep = zsocket_new( zmq_context, ZMQ_REP );
    zsocket_bind( zmq_input_rep, "inproc://input" );

    RenderThreadParms render_thread_parms;
    render_thread_parms.root = ogre_root;
    render_thread_parms.window = window;
    render_thread_parms.gl_context = glcontext;
    render_thread_parms.ogre_window = ogre_render_window;
    render_thread_parms.zmq_context = zmq_context;
#ifdef __APPLE__
    OSX_GL_clear_current( ogre_render_window );
#else
    // NOTE: don't call this on OSX, since SDL isn't managing the context!
    SDL_GL_MakeCurrent( window, NULL );
#endif
    SDL_Thread * sdl_render_thread = SDL_CreateThread( render_thread, "render", &render_thread_parms );

    SDL_SetWindowGrab( window, SDL_TRUE );
    SDL_SetRelativeMouseMode( SDL_TRUE );
  
    const int MAX_RUN_TIME = 60 * 1000; // shutdown the whole thing after some time
    bool shutdown_requested = false;
    InputState is;
    is.yaw_sens = 0.1f;
    is.yaw = 0.0f;
    is.pitch_sens = 0.1f;
    is.pitch = 0.0f;
    is.roll = 0.0f;
    while ( SDL_GetTicks() < MAX_RUN_TIME && !shutdown_requested ) {
      // we wait here
      char * input_request = zstr_recv( zmq_input_rep );
      // poll for events before processing the request
      // NOTE: this is how SDL builds the internal mouse and keyboard state
      // TODO: done this way does not meet the objectives of smooth, frame independent mouse view control,
      //   plus it throws some latency into the calling thread
      SDL_Event event;
      while ( SDL_PollEvent( &event ) ) {
	if ( event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ) {
	  printf( "SDL_KeyboardEvent\n" );
	  if ( event.type == SDL_KEYUP && ((SDL_KeyboardEvent*)&event)->keysym.scancode == SDL_SCANCODE_ESCAPE ) {
	    send_shutdown( zmq_render_socket, zmq_game_socket );
	    shutdown_requested = true;	    
	  }
	} else if ( event.type == SDL_MOUSEMOTION ) {
	  SDL_MouseMotionEvent * mev = (SDL_MouseMotionEvent*)&event;
	  is.yaw += is.yaw_sens * (float)mev->xrel;
	  if ( is.yaw >= 0.0f ) {
	    is.yaw = fmod( is.yaw + 180.0f, 360.0f ) - 180.0f;
	  } else {
	    is.yaw = fmod( is.yaw - 180.0f, 360.0f ) + 180.0f;
	  }
	  is.pitch += is.pitch_sens * (float)mev->yrel;
	  if ( is.pitch > 90.0f ) {
	    is.pitch = 90.0f;
	  } else if ( is.pitch < -90.0f ) {
	    is.pitch = -90.0f;
	  }
	  // build a quaternion of the current orientation
	  Ogre::Matrix3 r;
	  r.FromEulerAnglesYXZ( Ogre::Radian( Ogre::Degree( is.yaw ) ), Ogre::Radian( Ogre::Degree( is.pitch ) ), Ogre::Radian( Ogre::Degree( is.roll ) ) );
	  is.orientation.FromRotationMatrix( r );
	} else if ( event.type == SDL_MOUSEBUTTONUP || event.type == SDL_MOUSEBUTTONDOWN ) {
	  printf( "SDL_MouseButtonEvent\n" );
	} else if ( event.type == SDL_QUIT ) {
	  printf( "SDL_Quit\n" );
	  // push a shutdown on the control socket, game and render will pick it up later
	  // NOTE: if the req/rep patterns change we may still have to deal with hangs here
	  send_shutdown( zmq_render_socket, zmq_game_socket );
	  shutdown_requested = true;
	} else {
	  printf( "SDL_Event %d\n", event.type );
	}
      }
      // we are ready to process the request now
      if ( strcmp( input_request, "mouse_state" ) == 0 ) {
	int x, y;
	Uint8 buttons = SDL_GetMouseState( &x, &y );
	zstr_sendf( zmq_input_rep, "%f %f %f %f %d", is.orientation.w, is.orientation.x, is.orientation.y, is.orientation.z, buttons );
      } else if ( strncmp( input_request, "mouse_reset", strlen( "mouse_reset" ) ) == 0 ) {
	// reset the orientation
	parse_orientation( input_request + strlen( "mouse_reset" ) + 1, is.orientation );

	Ogre::Matrix3 r;
	is.orientation.ToRotationMatrix( r );
	Ogre::Radian rfYAngle, rfPAngle, rfRAngle;
	r.ToEulerAnglesYXZ( rfYAngle, rfPAngle, rfRAngle );
	is.yaw = rfYAngle.valueDegrees();
	is.pitch = rfPAngle.valueDegrees();
	is.roll = rfRAngle.valueDegrees();

	zstr_send( zmq_input_rep, "" ); // nop (acknowledge)
      } else {
	zstr_send( zmq_input_rep, "" ); // nop
      }
      free( input_request );
    }

    if ( !shutdown_requested ) {
      // NOTE: the game thread could still hang between the send and the wait if it issues a req/rep in between
      send_shutdown( zmq_render_socket, zmq_game_socket );
      shutdown_requested = true;
    }
    wait_shutdown( sdl_render_thread, sdl_game_thread );

    zctx_destroy( &zmq_context );
    // make the GL context again before proceeding with the teardown
#ifdef __APPLE__
    OSX_GL_set_current( ogre_render_window );
#else
    SDL_GL_MakeCurrent( window, glcontext );
#endif
    delete ogre_root;

  } catch ( Ogre::Exception &e ) {
    printf( "Ogre::Exception %s\n", e.what() );
    retcode = -1;
  }

#ifndef __WINDOWS__
  // FIXME: Windows crashes in the OpenGL drivers inside the DestroyWindow() call ?
  SDL_Quit();
#endif

  return retcode;
}
