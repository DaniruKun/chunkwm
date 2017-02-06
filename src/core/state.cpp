#include "state.h"

#include "dispatch/event.h"

#include "../common/accessibility/application.h"
#include "../common/accessibility/window.h"
#include "../common/accessibility/element.h"

#include <pthread.h>

#include <map>
#include <vector>

#define internal static

typedef std::map<pid_t, macos_application *> macos_application_map;
typedef macos_application_map::iterator macos_application_map_it;

typedef std::map<uint32_t, macos_window *> macos_window_map;
typedef macos_window_map::iterator macos_window_map_it;

internal macos_application_map Applications;

internal macos_window_map Windows;
internal pthread_mutex_t WindowsLock;

/* NOTE(koekeishiya): We need a way to retrieve AXUIElementRef from a CGWindowID.
 * There is no way to do this, without caching AXUIElementRef references.
 * Here we perform a lookup of macos_window structs. */
internal macos_window *
GetWindowByID(uint32_t Id)
{
    pthread_mutex_lock(&WindowsLock);
    macos_window_map_it It = Windows.find(Id);
    macos_window *Result = (It != Windows.end()) ? It->second : NULL;
    pthread_mutex_unlock(&WindowsLock);

    return Result;
}

/* NOTE(koekeishiya): Caller is responsible for making sure that the window is not a dupe.
 * If the window can not be added to the collection, caller is responsible for memory. */
bool AddWindowToCollection(macos_window *Window)
{
    // NOTE(koekeishiya): A window with id 0 is never valid!
    if(Window->Id == 0)
    {
        return false;
    }

    AXError Success = AXLibAddObserverNotification(&Window->Owner->Observer,
                                                   Window->Ref,
                                                   kAXUIElementDestroyedNotification,
                                                   Window);
    bool Result = (Success == kAXErrorSuccess);
    if(Result)
    {
        pthread_mutex_lock(&WindowsLock);
        Windows[Window->Id] = Window;
        pthread_mutex_unlock(&WindowsLock);
    }

    return Result;
}

// NOTE(koekeishiya): Caller is responsible for passing a valid window!
void RemoveWindowFromCollection(macos_window *Window)
{
    pthread_mutex_lock(&WindowsLock);
    Windows.erase(Window->Id);
    pthread_mutex_unlock(&WindowsLock);

    AXLibRemoveObserverNotification(&Window->Owner->Observer, Window->Ref, kAXUIElementDestroyedNotification);
}

// NOTE(koekeishiya): Caller is responsible for passing a valid window!
void UpdateWindowTitle(macos_window *Window)
{
    if(Window->Name)
    {
        free(Window->Name);
    }

    Window->Name = AXLibGetWindowTitle(Window->Ref);
}

/* NOTE(koekeishiya): Construct macos_windows for an application and add them to our window-collection.
 * If a window is not added to our collection for any reason, we release the memory. */
internal void
AddApplicationWindowsToCollection(macos_application *Application)
{
    macos_window **WindowList = AXLibWindowListForApplication(Application);
    if(WindowList)
    {
        macos_window *Window = NULL;
        macos_window **List = WindowList;

        while((Window = *List++))
        {
            if(GetWindowByID(Window->Id))
            {
                AXLibDestroyWindow(Window);
            }
            else
            {
                if(!AddWindowToCollection(Window))
                {
                    printf("%s:%s is not destructible, ignore!\n", Window->Owner->Name, Window->Name);
                    AXLibDestroyWindow(Window);
                }
            }
        }

        free(WindowList);
    }
}

// NOTE(koekeishiya): We need a way to retrieve a macos_application * by PID
macos_application *GetApplicationFromPID(pid_t PID)
{
    macos_application *Result = NULL;

    macos_application_map_it It = Applications.find(PID);
    if(It != Applications.end())
    {
        Result = It->second;
    }

    return Result;
}

internal void
AddApplication(macos_application *Application)
{
    macos_application_map_it It = Applications.find(Application->PID);
    if(It == Applications.end())
    {
        Applications[Application->PID] = Application;
    }
}

internal
OBSERVER_CALLBACK(ApplicationCallback)
{
    macos_application *Application = (macos_application *) Reference;

    if(CFEqual(Notification, kAXWindowCreatedNotification))
    {
        macos_window *Window = AXLibConstructWindow(Application, Element);
        ConstructEvent(ChunkWM_WindowCreated, Window);
    }
    else if(CFEqual(Notification, kAXUIElementDestroyedNotification))
    {
        /* NOTE(koekeishiya): If this is an actual window, it should be associated
         * with a valid CGWindowID. HOWEVER, because the window in question has been
         * destroyed. We are unable to utilize this window reference with the AX API.
         *
         * The 'CFEqual()' function can still be used to compare this AXUIElementRef
         * with any existing window refs that we may have. There are a couple of ways
         * we can use to track if an actual window is closed.
         *
         *   a) Store all window AXUIElementRefs in a local cache that we update upon
         *      creation and removal. Requires unsorted container with custom comparator
         *      that uses 'CFEqual()' to match AXUIElementRefs.
         *
         *   b) Instead of tracking 'kAXUIElementDestroyedNotification' for an application,
         *      we have to register this notification separately for every window created.
         *      By doing this, we can pass our own data containing the information necessary
         *      to properly identify and report which window was destroyed.
         *
         * At the very least, we need to know the windowid of the destroyed window. */

        /* NOTE(koekeishiya): Option 'b' has been implemented. Leaving note for future reference. */

        macos_window *Window = (macos_window *) Reference;
        ConstructEvent(ChunkWM_WindowDestroyed, Window);
    }
    else if(CFEqual(Notification, kAXFocusedWindowChangedNotification))
    {
        uint32_t WindowId = AXLibGetWindowID(Element);
        macos_window *Window = GetWindowByID(WindowId);
        if(Window)
        {
            ConstructEvent(ChunkWM_WindowFocused, Window);
        }
    }
    else if(CFEqual(Notification, kAXWindowMovedNotification))
    {
        uint32_t WindowId = AXLibGetWindowID(Element);
        macos_window *Window = GetWindowByID(WindowId);
        if(Window)
        {
            ConstructEvent(ChunkWM_WindowMoved, Window);
        }
    }
    else if(CFEqual(Notification, kAXWindowResizedNotification))
    {
        uint32_t WindowId = AXLibGetWindowID(Element);
        macos_window *Window = GetWindowByID(WindowId);
        if(Window)
        {
            ConstructEvent(ChunkWM_WindowResized, Window);
        }
    }
    else if(CFEqual(Notification, kAXWindowMiniaturizedNotification))
    {
        uint32_t WindowId = AXLibGetWindowID(Element);
        macos_window *Window = GetWindowByID(WindowId);
        if(Window)
        {
            ConstructEvent(ChunkWM_WindowMinimized, Window);
        }
    }
    else if(CFEqual(Notification, kAXWindowDeminiaturizedNotification))
    {
        uint32_t WindowId = AXLibGetWindowID(Element);
        macos_window *Window = GetWindowByID(WindowId);
        if(Window)
        {
            ConstructEvent(ChunkWM_WindowDeminimized, Window);
        }
    }
    else if(CFEqual(Notification, kAXTitleChangedNotification))
    {
        uint32_t WindowId = AXLibGetWindowID(Element);
        macos_window *Window = GetWindowByID(WindowId);
        if(Window)
        {
            ConstructEvent(ChunkWM_WindowTitleChanged, Window);
        }
    }
}

#define MICROSEC_PER_SEC 1e6
macos_application *ConstructAndAddApplication(ProcessSerialNumber PSN, pid_t PID, char *ProcessName)
{
    macos_application *Application = AXLibConstructApplication(PSN, PID, ProcessName);
    AddApplication(Application);

    /* NOTE(koekeishiya): We need to wait for some amount of time before we can try to
     * observe the launched application. The time to wait depends on how long the
     * application in question takes to finish. Half a second is good enough for
     * most applications so we 'usleep()' as a temporary fix for now, but we need a way
     * to properly defer the creation of observers for applications that require more time.
     *
     * We cannot simply defer the creation automatically using dispatch_after, because
     * there is simply no way to remove a dispatched event once it has been created.
     * We need a way to tell a dispatched event to NOT execute and be rendered invalid,
     * because some applications only live for a very very short amount of time.
     * The dispatched event will then be triggered after a potential 'terminated' event
     * has been received, in which the application reference has been freed.
     *
     * Passing an invalid reference to the AXObserver API does not simply trigger an error,
     * but causes a full on segmentation fault. */

    usleep(0.5 * MICROSEC_PER_SEC);
    if(AXLibAddApplicationObserver(Application, ApplicationCallback))
    {
        printf("%d:%s registered window notifications\n", Application->PID, Application->Name);
    }
    else
    {
        printf("%d:%s could not register window notifications!!!\n", Application->PID, Application->Name);
        printf("%d:%s could not register window notifications!!!\n", Application->PID, Application->Name);
        printf("%d:%s could not register window notifications!!!\n", Application->PID, Application->Name);
    }

    // NOTE(koekeishiya): An application can have multiple windows when it spawns.
    // We need to track all of these.
    AddApplicationWindowsToCollection(Application);

    return Application;
}

void RemoveAndDestroyApplication(macos_application *Application)
{
    macos_application_map_it It = Applications.find(Application->PID);
    if(It != Applications.end())
    {
        Applications.erase(It);
    }

    AXLibDestroyApplication(Application);
}

void UpdateWindowCollection()
{
    for(macos_application_map_it It = Applications.begin();
        It != Applications.end();
        ++It)
    {
        macos_application *Application = It->second;
        AddApplicationWindowsToCollection(Application);
    }
}

// NOTE(koekeishiya): This function is only supposed to be called by our chunkwm main function
bool InitState()
{
    bool Result = pthread_mutex_init(&WindowsLock, NULL) == 0;
    if(Result)
    {
        uint32_t ProcessPolicy = Process_Policy_Regular | Process_Policy_LSUIElement;
        std::vector<macos_application *> RunningApplications = AXLibRunningProcesses(ProcessPolicy);

        for(size_t Index = 0;
            Index < RunningApplications.size();
            ++Index)
        {
            macos_application *Application = RunningApplications[Index];
            AddApplication(Application);
            AXLibAddApplicationObserver(Application, ApplicationCallback);
            AddApplicationWindowsToCollection(Application);
        }
    }

    return Result;
}