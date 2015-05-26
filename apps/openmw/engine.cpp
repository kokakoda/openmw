#include "engine.hpp"

#include <stdexcept>
#include <iomanip>

#include <boost/filesystem/fstream.hpp>

#include <osgViewer/ViewerEventHandlers>
#include <osgDB/ReadFile>

#include <SDL.h>

#include <components/misc/rng.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <components/sdlutil/sdlgraphicswindow.hpp>
#include <components/sdlutil/imagetosurface.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/texturemanager.hpp>

#include <components/compiler/extensions0.hpp>

#include <components/files/configurationmanager.hpp>
#include <components/translation/translation.hpp>

#include <components/esm/loadcell.hpp>

#include "mwinput/inputmanagerimp.hpp"

#include "mwgui/windowmanagerimp.hpp"

#include "mwscript/scriptmanagerimp.hpp"
#include "mwscript/extensions.hpp"
#include "mwscript/interpretercontext.hpp"

#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/player.hpp"
#include "mwworld/worldimp.hpp"

#include "mwrender/vismask.hpp"

#include "mwclass/classes.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/mechanicsmanagerimp.hpp"

#include "mwstate/statemanagerimp.hpp"

void OMW::Engine::executeLocalScripts()
{
    MWWorld::LocalScripts& localScripts = MWBase::Environment::get().getWorld()->getLocalScripts();

    localScripts.startIteration();

    while (!localScripts.isFinished())
    {
        std::pair<std::string, MWWorld::Ptr> script = localScripts.getNext();

        MWScript::InterpreterContext interpreterContext (
            &script.second.getRefData().getLocals(), script.second);
        MWBase::Environment::get().getScriptManager()->run (script.first, interpreterContext);
    }

    localScripts.setIgnore (MWWorld::Ptr());
}

double OMW::Engine::frame(float frametime)
{
    try
    {
        mStartTick = mViewer->getStartTick();
        mEnvironment.setFrameDuration (frametime);

        // update input
        MWBase::Environment::get().getInputManager()->update(frametime, false);

        // When the window is minimized, pause everything. Currently this *has* to be here to work around a MyGUI bug.
        // If we are not currently rendering, then RenderItems will not be reused resulting in a memory leak upon changing widget textures.
        //if (!mOgre->getWindow()->isActive() || !mOgre->getWindow()->isVisible())
        //    return true;

        // sound
        if (mUseSound)
            MWBase::Environment::get().getSoundManager()->update(frametime);

        // GUI active? Most game processing will be paused, but scripts still run.
        bool guiActive = MWBase::Environment::get().getWindowManager()->isGuiMode();
        if (!guiActive)
            mSimulationTime += frametime;

        // Main menu opened? Then scripts are also paused.
        bool paused = MWBase::Environment::get().getWindowManager()->containsMode(MWGui::GM_MainMenu);

        // update game state
        MWBase::Environment::get().getStateManager()->update (frametime);

        osg::Timer_t beforeScriptTick = osg::Timer::instance()->tick();
        if (MWBase::Environment::get().getStateManager()->getState()==
            MWBase::StateManager::State_Running)
        {
            if (!paused)
            {
                if (MWBase::Environment::get().getWorld()->getScriptsEnabled())
                {
                    // local scripts
                    executeLocalScripts();

                    // global scripts
                    MWBase::Environment::get().getScriptManager()->getGlobalScripts().run();
                }

                MWBase::Environment::get().getWorld()->markCellAsUnchanged();
            }

            if (!guiActive)
                MWBase::Environment::get().getWorld()->advanceTime(
                    frametime*MWBase::Environment::get().getWorld()->getTimeScaleFactor()/3600);
        }
        osg::Timer_t afterScriptTick = osg::Timer::instance()->tick();

        // update actors
        osg::Timer_t beforeMechanicsTick = osg::Timer::instance()->tick();
        if (MWBase::Environment::get().getStateManager()->getState()!=
            MWBase::StateManager::State_NoGame)
        {
            MWBase::Environment::get().getMechanicsManager()->update(frametime,
                guiActive);
        }
        osg::Timer_t afterMechanicsTick = osg::Timer::instance()->tick();

        if (MWBase::Environment::get().getStateManager()->getState()==
            MWBase::StateManager::State_Running)
        {
            MWWorld::Ptr player = mEnvironment.getWorld()->getPlayerPtr();
            if(!guiActive && player.getClass().getCreatureStats(player).isDead())
                MWBase::Environment::get().getStateManager()->endGame();
        }

        // update world
        osg::Timer_t beforePhysicsTick = osg::Timer::instance()->tick();;
        if (MWBase::Environment::get().getStateManager()->getState()!=
            MWBase::StateManager::State_NoGame)
        {
            MWBase::Environment::get().getWorld()->update(frametime, guiActive);
        }
        osg::Timer_t afterPhysicsTick = osg::Timer::instance()->tick();

        // update GUI
        MWBase::Environment::get().getWindowManager()->onFrame(frametime);
        if (MWBase::Environment::get().getStateManager()->getState()!=
            MWBase::StateManager::State_NoGame)
        {
#if 0
            MWBase::Environment::get().getWindowManager()->wmUpdateFps(fps);
#endif
            MWBase::Environment::get().getWindowManager()->update();
        }

        int frameNumber = mViewer->getFrameStamp()->getFrameNumber();
        osg::Stats* stats = mViewer->getViewerStats();
        stats->setAttribute(frameNumber, "script_time_begin", osg::Timer::instance()->delta_s(mStartTick, beforeScriptTick));
        stats->setAttribute(frameNumber, "script_time_taken", osg::Timer::instance()->delta_s(beforeScriptTick, afterScriptTick));
        stats->setAttribute(frameNumber, "script_time_end", osg::Timer::instance()->delta_s(mStartTick, afterScriptTick));

        stats->setAttribute(frameNumber, "mechanics_time_begin", osg::Timer::instance()->delta_s(mStartTick, beforeMechanicsTick));
        stats->setAttribute(frameNumber, "mechanics_time_taken", osg::Timer::instance()->delta_s(beforeMechanicsTick, afterMechanicsTick));
        stats->setAttribute(frameNumber, "mechanics_time_end", osg::Timer::instance()->delta_s(mStartTick, afterMechanicsTick));

        stats->setAttribute(frameNumber, "physics_time_begin", osg::Timer::instance()->delta_s(mStartTick, beforePhysicsTick));
        stats->setAttribute(frameNumber, "physics_time_taken", osg::Timer::instance()->delta_s(beforePhysicsTick, afterPhysicsTick));
        stats->setAttribute(frameNumber, "physics_time_end", osg::Timer::instance()->delta_s(mStartTick, afterPhysicsTick));

    }
    catch (const std::exception& e)
    {
        std::cerr << "Error in framelistener: " << e.what() << std::endl;
    }
    return mSimulationTime;
}

OMW::Engine::Engine(Files::ConfigurationManager& configurationManager)
  : mWindow(NULL)
  , mEncoding(ToUTF8::WINDOWS_1252)
  , mEncoder(NULL)
  , mVerboseScripts (false)
  , mSkipMenu (false)
  , mUseSound (true)
  , mCompileAll (false)
  , mCompileAllDialogue (false)
  , mWarningsMode (1)
  , mScriptConsoleMode (false)
  , mActivationDistanceOverride(-1)
  , mGrab(true)
  , mExportFonts(false)
  , mScriptContext (0)
  , mFSStrict (false)
  , mScriptBlacklistUse (true)
  , mNewGame (false)
  , mSimulationTime(0.0)
  , mCfgMgr(configurationManager)
{
    Misc::Rng::init();
    MWClass::registerClasses();

    Uint32 flags = SDL_INIT_VIDEO|SDL_INIT_NOPARACHUTE|SDL_INIT_GAMECONTROLLER|SDL_INIT_JOYSTICK;
    if(SDL_WasInit(flags) == 0)
    {
        SDL_SetMainReady();
        if(SDL_Init(flags) != 0)
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
    }

    mStartTick = osg::Timer::instance()->tick();
}

OMW::Engine::~Engine()
{
    mEnvironment.cleanup();

    delete mScriptContext;
    mScriptContext = NULL;

    mResourceSystem.reset();

    mViewer = NULL;

    if (mWindow)
    {
        SDL_DestroyWindow(mWindow);
        mWindow = NULL;
    }

    SDL_Quit();
}

void OMW::Engine::enableFSStrict(bool fsStrict)
{
    mFSStrict = fsStrict;
}

// Set data dir

void OMW::Engine::setDataDirs (const Files::PathContainer& dataDirs)
{
    mDataDirs = dataDirs;
    mFileCollections = Files::Collections (dataDirs, !mFSStrict);
}

// Add BSA archive
void OMW::Engine::addArchive (const std::string& archive) {
    mArchives.push_back(archive);
}

// Set resource dir
void OMW::Engine::setResourceDir (const boost::filesystem::path& parResDir)
{
    mResDir = parResDir;
}

// Set start cell name (only interiors for now)

void OMW::Engine::setCell (const std::string& cellName)
{
    mCellName = cellName;
}

void OMW::Engine::addContentFile(const std::string& file)
{
    mContentFiles.push_back(file);
}

void OMW::Engine::setScriptsVerbosity(bool scriptsVerbosity)
{
    mVerboseScripts = scriptsVerbosity;
}

void OMW::Engine::setSkipMenu (bool skipMenu, bool newGame)
{
    mSkipMenu = skipMenu;
    mNewGame = newGame;
}

std::string OMW::Engine::loadSettings (Settings::Manager & settings)
{
    // Create the settings manager and load default settings file
    const std::string localdefault = mCfgMgr.getLocalPath().string() + "/settings-default.cfg";
    const std::string globaldefault = mCfgMgr.getGlobalPath().string() + "/settings-default.cfg";

    // prefer local
    if (boost::filesystem::exists(localdefault))
        settings.loadDefault(localdefault);
    else if (boost::filesystem::exists(globaldefault))
        settings.loadDefault(globaldefault);
    else
        throw std::runtime_error ("No default settings file found! Make sure the file \"settings-default.cfg\" was properly installed.");

    // load user settings if they exist
    const std::string settingspath = mCfgMgr.getUserConfigPath().string() + "/settings.cfg";
    if (boost::filesystem::exists(settingspath))
        settings.loadUser(settingspath);

    return settingspath;
}

void OMW::Engine::createWindow(Settings::Manager& settings)
{
    int screen = settings.getInt("screen", "Video");
    int width = settings.getInt("resolution x", "Video");
    int height = settings.getInt("resolution y", "Video");
    bool fullscreen = settings.getBool("fullscreen", "Video");
    bool windowBorder = settings.getBool("window border", "Video");
    bool vsync = settings.getBool("vsync", "Video");
    int antialiasing = settings.getInt("antialiasing", "Video");

    int pos_x = SDL_WINDOWPOS_CENTERED_DISPLAY(screen),
        pos_y = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);

    if(fullscreen)
    {
        pos_x = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        pos_y = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
    }

    Uint32 flags = SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE;
    if(fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;

    if (!windowBorder)
        flags |= SDL_WINDOW_BORDERLESS;

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS,
                settings.getBool("minimize on focus loss", "Video") ? "1" : "0");

    if (antialiasing > 0)
    {
        if (SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1) != 0)
            std::cerr << "SDL error: " << SDL_GetError() << std::endl;
        if (SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing) != 0)
            std::cerr << "SDL error: " << SDL_GetError() << std::endl;
    }

    mWindow = SDL_CreateWindow("OpenMW", pos_x, pos_y, width, height, flags);
    if (mWindow == NULL)
    {
        std::cerr << "Failed to create SDL window: " << SDL_GetError() << std::endl;
        return;
    }

    setWindowIcon();

    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    SDL_GetWindowPosition(mWindow, &traits->x, &traits->y);
    SDL_GetWindowSize(mWindow, &traits->width, &traits->height);
    traits->windowName = SDL_GetWindowTitle(mWindow);
    traits->windowDecoration = !(SDL_GetWindowFlags(mWindow)&SDL_WINDOW_BORDERLESS);
    traits->screenNum = SDL_GetWindowDisplayIndex(mWindow);
    // FIXME: Some way to get these settings back from the SDL window?
    traits->red = 8;
    traits->green = 8;
    traits->blue = 8;
    traits->alpha = 8;
    traits->depth = 24;
    traits->stencil = 8;
    traits->vsync = vsync;
    traits->doubleBuffer = true;
    traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(mWindow);

    osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> graphicsWindow = new SDLUtil::GraphicsWindowSDL2(traits);
    if(!graphicsWindow->valid()) throw std::runtime_error("Failed to create GraphicsContext");

    osg::ref_ptr<osg::Camera> camera = mViewer->getCamera();
    camera->setGraphicsContext(graphicsWindow);
    camera->setViewport(0, 0, width, height);

    mViewer->realize();
}

void OMW::Engine::setWindowIcon()
{
    boost::filesystem::ifstream windowIconStream;
    std::string windowIcon = (mResDir / "mygui" / "openmw.png").string();
    windowIconStream.open(windowIcon);
    if (windowIconStream.fail())
        std::cerr << "Failed to open " << windowIcon << std::endl;
    osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
    osgDB::ReaderWriter::ReadResult result = reader->readImage(windowIconStream);
    if (!result.success())
        std::cerr << "Failed to read " << windowIcon << ": " << result.message() << std::endl;
    else
    {
        osg::ref_ptr<osg::Image> image = result.getImage();
        SDL_Surface* surface = SDLUtil::imageToSurface(image, true);
        SDL_SetWindowIcon(mWindow, surface);
        SDL_FreeSurface(surface);
    }
}

void OMW::Engine::prepareEngine (Settings::Manager & settings)
{
    mEnvironment.setStateManager (
        new MWState::StateManager (mCfgMgr.getUserDataPath() / "saves", mContentFiles.at (0)));

    createWindow(settings);

    osg::ref_ptr<osg::Group> rootNode (new osg::Group);
    mViewer->setSceneData(rootNode);

    mVFS.reset(new VFS::Manager(mFSStrict));

    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true);

    mResourceSystem.reset(new Resource::ResourceSystem(mVFS.get()));
    mResourceSystem->getTextureManager()->setUnRefImageDataAfterApply(true);
    osg::Texture::FilterMode min = osg::Texture::LINEAR_MIPMAP_NEAREST;
    osg::Texture::FilterMode mag = osg::Texture::LINEAR;
    if (Settings::Manager::getString("texture filtering", "General") == "trilinear")
        min = osg::Texture::LINEAR_MIPMAP_LINEAR;
    int maxAnisotropy = Settings::Manager::getInt("anisotropy", "General");
    mResourceSystem->getTextureManager()->setFilterSettings(min, mag, maxAnisotropy);

    // Create input and UI first to set up a bootstrapping environment for
    // showing a loading screen and keeping the window responsive while doing so

    std::string keybinderUser = (mCfgMgr.getUserConfigPath() / "input_v3.xml").string();
    bool keybinderUserExists = boost::filesystem::exists(keybinderUser);
    if(!keybinderUserExists)
    {
        std::string input2 = (mCfgMgr.getUserConfigPath() / "input_v2.xml").string();
        if(boost::filesystem::exists(input2)) {
            boost::filesystem::copy_file(input2, keybinderUser);
            keybinderUserExists = boost::filesystem::exists(keybinderUser);
        }
    }

    // find correct path to the game controller bindings
    const std::string localdefault = mCfgMgr.getLocalPath().string() + "/gamecontrollerdb.cfg";
    const std::string globaldefault = mCfgMgr.getGlobalPath().string() + "/gamecontrollerdb.cfg";
    std::string gameControllerdb;
    if (boost::filesystem::exists(localdefault))
        gameControllerdb = localdefault;
    else if (boost::filesystem::exists(globaldefault))
        gameControllerdb = globaldefault;
    else
        gameControllerdb = ""; //if it doesn't exist, pass in an empty string

    // FIXME: shouldn't depend on Engine
    MWInput::InputManager* input = new MWInput::InputManager (mWindow, mViewer, *this, keybinderUser, keybinderUserExists, gameControllerdb, mGrab);
    mEnvironment.setInputManager (input);

    std::string myguiResources = (mResDir / "mygui").string();
    osg::ref_ptr<osg::Group> guiRoot = new osg::Group;
    guiRoot->setNodeMask(MWRender::Mask_GUI);
    rootNode->addChild(guiRoot);
    MWGui::WindowManager* window = new MWGui::WindowManager(mViewer, guiRoot, mResourceSystem.get(),
                mCfgMgr.getLogPath().string() + std::string("/"), myguiResources,
                mScriptConsoleMode, mTranslationDataStorage, mEncoding, mExportFonts, mFallbackMap);
    mEnvironment.setWindowManager (window);

    // Create sound system
    mEnvironment.setSoundManager (new MWSound::SoundManager(mVFS.get(), mUseSound));

    if (!mSkipMenu)
    {
        std::string logo = mFallbackMap["Movies_Company_Logo"];
        if (!logo.empty())
            window->playVideo(logo, true);
    }

    // Create the world
    mEnvironment.setWorld( new MWWorld::World (mViewer, rootNode, mResourceSystem.get(),
        mFileCollections, mContentFiles, mEncoder, mFallbackMap,
        mActivationDistanceOverride, mCellName, mStartupScript));
    MWBase::Environment::get().getWorld()->setupPlayer();
    input->setPlayer(&mEnvironment.getWorld()->getPlayer());

    window->initUI();
    window->renderWorldMap();

    //Load translation data
    mTranslationDataStorage.setEncoder(mEncoder);
    for (size_t i = 0; i < mContentFiles.size(); i++)
      mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFiles[i]);

    Compiler::registerExtensions (mExtensions);

    // Create script system
    mScriptContext = new MWScript::CompilerContext (MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions (&mExtensions);

    mEnvironment.setScriptManager (new MWScript::ScriptManager (MWBase::Environment::get().getWorld()->getStore(),
        mVerboseScripts, *mScriptContext, mWarningsMode,
        mScriptBlacklistUse ? mScriptBlacklist : std::vector<std::string>()));

    // Create game mechanics system
    MWMechanics::MechanicsManager* mechanics = new MWMechanics::MechanicsManager;
    mEnvironment.setMechanicsManager (mechanics);

    // Create dialog system
    mEnvironment.setJournal (new MWDialogue::Journal);
    mEnvironment.setDialogueManager (new MWDialogue::DialogueManager (mExtensions, mVerboseScripts, mTranslationDataStorage));

    // scripts
    if (mCompileAll)
    {
        std::pair<int, int> result = MWBase::Environment::get().getScriptManager()->compileAll();
        if (result.first)
            std::cout
                << "compiled " << result.second << " of " << result.first << " scripts ("
                << 100*static_cast<double> (result.second)/result.first
                << "%)"
                << std::endl;
    }
    if (mCompileAllDialogue)
    {
        std::pair<int, int> result = MWDialogue::ScriptTest::compileAll(&mExtensions, mWarningsMode);
        if (result.first)
            std::cout
                << "compiled " << result.second << " of " << result.first << " dialogue script/actor combinations a("
                << 100*static_cast<double> (result.second)/result.first
                << "%)"
                << std::endl;
    }
}

// Initialise and enter main loop.

void OMW::Engine::go()
{
    assert (!mContentFiles.empty());

    mViewer = new osgViewer::Viewer;

    osg::ref_ptr<osgViewer::StatsHandler> statshandler = new osgViewer::StatsHandler;
    statshandler->setKeyEventTogglesOnScreenStats(osgGA::GUIEventAdapter::KEY_F9);

    statshandler->addUserStatsLine("Script", osg::Vec4f(1.f, 1.f, 1.f, 1.f), osg::Vec4f(1.f, 1.f, 1.f, 1.f),
                                   "script_time_taken", 1000.0, true, false, "script_time_begin", "script_time_end", 10000);
    statshandler->addUserStatsLine("Mechanics", osg::Vec4f(1.f, 1.f, 1.f, 1.f), osg::Vec4f(1.f, 1.f, 1.f, 1.f),
                                   "mechanics_time_taken", 1000.0, true, false, "mechanics_time_begin", "mechanics_time_end", 10000);
    statshandler->addUserStatsLine("Physics", osg::Vec4f(1.f, 1.f, 1.f, 1.f), osg::Vec4f(1.f, 1.f, 1.f, 1.f),
                                   "physics_time_taken", 1000.0, true, false, "physics_time_begin", "physics_time_end", 10000);

    mViewer->addEventHandler(statshandler);

    Settings::Manager settings;
    std::string settingspath;

    settingspath = loadSettings (settings);

    // Create encoder
    ToUTF8::Utf8Encoder encoder (mEncoding);
    mEncoder = &encoder;

    prepareEngine (settings);

    // Play some good 'ol tunes
    MWBase::Environment::get().getSoundManager()->playPlaylist(std::string("Explore"));

    if (!mSaveGameFile.empty())
    {
        MWBase::Environment::get().getStateManager()->loadGame(mSaveGameFile);
    }
    else if (!mSkipMenu)
    {
        // start in main menu
        MWBase::Environment::get().getWindowManager()->pushGuiMode (MWGui::GM_MainMenu);
        try
        {
            // Is there an ini setting for this filename or something?
            MWBase::Environment::get().getSoundManager()->streamMusic("Special/morrowind title.mp3");

            std::string logo = mFallbackMap["Movies_Morrowind_Logo"];
            if (!logo.empty())
                MWBase::Environment::get().getWindowManager()->playVideo(logo, true);
        }
        catch (...) {}
    }
    else
    {
        MWBase::Environment::get().getStateManager()->newGame (!mNewGame);
    }

    // Start the main rendering loop
    osg::Timer frameTimer;
    while (!mViewer->done() && !MWBase::Environment::get().getStateManager()->hasQuitRequest())
    {
        double dt = frameTimer.time_s();
        frameTimer.setStartTick();
        dt = std::min(dt, 0.2);

        double simulationTime = frame(dt);
        mViewer->frame(simulationTime);
    }

    // Save user settings
    settings.saveUser(settingspath);

    std::cout << "Quitting peacefully." << std::endl;
}

void OMW::Engine::activate()
{
    if (MWBase::Environment::get().getWindowManager()->isGuiMode())
        return;

    MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
    if (player.getClass().getCreatureStats(player).getMagicEffects().get(ESM::MagicEffect::Paralyze).getMagnitude() > 0
            || player.getClass().getCreatureStats(player).getKnockedDown())
        return;

    MWWorld::Ptr ptr = MWBase::Environment::get().getWorld()->getFacedObject();

    if (ptr.isEmpty())
        return;

    if (ptr.getClass().getName(ptr) == "") // objects without name presented to user can never be activated
        return;

    if (ptr.getClass().isActor())
    {
        MWMechanics::CreatureStats &stats = ptr.getClass().getCreatureStats(ptr);

        if (stats.getAiSequence().isInCombat() && !stats.isDead())
            return;
    }

    MWBase::Environment::get().getWorld()->activate(ptr, MWBase::Environment::get().getWorld()->getPlayerPtr());
}

void OMW::Engine::screenshot()
{
    // Count screenshots.
    int shotCount = 0;

    const std::string& screenshotPath = mCfgMgr.getUserDataPath().string();
    std::string format = Settings::Manager::getString("screenshot format", "General");
    // Find the first unused filename with a do-while
    std::ostringstream stream;
    do
    {
        // Reset the stream
        stream.str("");
        stream.clear();

        stream << screenshotPath << "screenshot" << std::setw(3) << std::setfill('0') << shotCount++ << "." << format;

    } while (boost::filesystem::exists(stream.str()));

    //mOgre->screenshot(stream.str(), format);
}

void OMW::Engine::setCompileAll (bool all)
{
    mCompileAll = all;
}

void OMW::Engine::setCompileAllDialogue (bool all)
{
    mCompileAllDialogue = all;
}

void OMW::Engine::setSoundUsage(bool soundUsage)
{
    mUseSound = soundUsage;
}

void OMW::Engine::setEncoding(const ToUTF8::FromType& encoding)
{
    mEncoding = encoding;
}

void OMW::Engine::setFallbackValues(std::map<std::string,std::string> fallbackMap)
{
    mFallbackMap = fallbackMap;
}

void OMW::Engine::setScriptConsoleMode (bool enabled)
{
    mScriptConsoleMode = enabled;
}

void OMW::Engine::setStartupScript (const std::string& path)
{
    mStartupScript = path;
}

void OMW::Engine::setActivationDistanceOverride (int distance)
{
    mActivationDistanceOverride = distance;
}

void OMW::Engine::setWarningsMode (int mode)
{
    mWarningsMode = mode;
}

void OMW::Engine::setScriptBlacklist (const std::vector<std::string>& list)
{
    mScriptBlacklist = list;
}

void OMW::Engine::setScriptBlacklistUse (bool use)
{
    mScriptBlacklistUse = use;
}

void OMW::Engine::enableFontExport(bool exportFonts)
{
    mExportFonts = exportFonts;
}

void OMW::Engine::setSaveGameFile(const std::string &savegame)
{
    mSaveGameFile = savegame;
}
