// « Signaler un problème » in the Windows launcher, as the Linux panel's
// (bdex-framegen --report / --issue-url): a diagnostic report written to a
// file, and the link to the pre-filled issue form.
#pragma once
#include <string>

namespace bdex {

// The report's text: version, Windows, GPUs, the layer's registration, the
// games with an injection, what runs now (the status files), the settings
// file and the end of the log file (log_file).
std::string buildReport();
// The issue form's link from a report (the same fields as the Linux one:
// never the settings nor the log, which stay in the attached file).
std::string issueUrl(const std::string& report);
// Writes buildReport() to %LOCALAPPDATA%\bdex-framegen\bdex-framegen-report.txt:
// its path ("" on failure), and the form's link in `url`.
std::wstring writeReport(std::string& url);

} // namespace bdex
