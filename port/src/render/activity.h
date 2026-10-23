// macOS's App Nap would slow the run while its window cannot be seen. keep_running() exempts the
// process for as long as it runs, and for an unattended run, such as an unthrottled one, also
// keeps the Mac from sleeping when idle.
#pragma once

namespace render {

void keep_running(bool unattended);

}  // namespace render
