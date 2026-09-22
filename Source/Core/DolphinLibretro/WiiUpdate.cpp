// RetroXR: install the Wii System Menu (and the IOS it runs on) into the NAND
// from Nintendo's update servers, without booting anything. It is Dolphin's own
// Tools > Perform Online System Update, exported for a frontend that loads its
// own copy of the core to call it: no retro_init, no environment callback.
//
// Done here and not in the frontend because installing a title means decrypting
// it with the Wii common key, which Dolphin already carries and RetroXR must not.
//
// Nintendo's Wii update service is offline; Dolphin's server lists the titles
// and the Wii U CDN serves them (WiiUtils::OnlineSystemUpdater).

#include <cstddef>
#include <cstdint>
#include <string>

#include <libretro.h>

#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Core/WiiUtils.h"
#include "UICommon/UICommon.h"

// processed/total count titles; returning 0 cancels once the current title is in.
typedef int (*retroxr_wii_update_progress_t)(void* userdata, size_t processed, size_t total,
                                             uint64_t title_id);

// user_dir is the directory holding Wii/ (the core's <save>/User), sys_dir its
// dolphin-emu/Sys. region is "USA", "EUR", "JPN" or "KOR"; empty keeps the region
// of an installed menu. Returns a WiiUtils::UpdateResult: 0 installed,
// 1 already up to date, anything else failed (see WiiUtils.h).
extern "C" RETRO_API int retroxr_wii_system_update(const char* user_dir, const char* sys_dir,
                                                   const char* region,
                                                   retroxr_wii_update_progress_t progress,
                                                   void* userdata)
{
  if (!user_dir || !*user_dir || !sys_dir || !*sys_dir)
    return static_cast<int>(WiiUtils::UpdateResult::ImportFailed);

  // A panic alert would open a message box on a thread nobody is looking at.
  Common::SetEnableAlert(false);
  File::SetSysDirectory(sys_dir);
  UICommon::SetUserDirectory(user_dir);
  UICommon::CreateDirectories();
  UICommon::Init();

  const WiiUtils::UpdateResult result = WiiUtils::DoOnlineUpdate(
      [progress, userdata](size_t processed, size_t total, u64 title_id) {
        return !progress || progress(userdata, processed, total, title_id) != 0;
      },
      region ? region : "");

  UICommon::Shutdown();
  return static_cast<int>(result);
}
