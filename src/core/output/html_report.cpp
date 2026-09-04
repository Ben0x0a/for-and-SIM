#include "html_report.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "report_common.h"
#include "version.h"

namespace forandsim::output {

namespace {

std::string escapeHtml(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string fileSizeOf(const ExtractedFile& f) {
    size_t size = f.rawData.size();
    for (auto& r : f.records) size += r.size();
    return std::to_string(size);
}

} // namespace

void writeHtmlReport(const AcquisitionResult& result, const std::string& htmlPath,
                      const std::string& zipFileName, const EvidenceZipResult& zipInfo) {
    std::ostringstream html;

    html << "<!doctype html><html><head><meta charset=\"utf-8\">"
         << "<title>SIM Acquisition Report - " << escapeHtml(result.caseMetadata.caseIdentifier)
         << "</title><style>"
         << "body{font-family:-apple-system,Segoe UI,Arial,sans-serif;margin:2rem;color:#1a1a1a;}"
         << "h1{font-size:1.4rem;} h2{font-size:1.1rem;margin-top:2rem;border-bottom:1px solid #ccc;padding-bottom:.25rem;}"
         << "h3{font-size:.95rem;margin-top:1.25rem;}"
         << "table{border-collapse:collapse;width:100%;margin-top:.5rem;}"
         << "th,td{border:1px solid #ddd;padding:.4rem .6rem;text-align:left;font-size:.9rem;}"
         << "th{background:#f0f0f0;} tr:nth-child(even){background:#fafafa;}"
         << "code{font-family:Menlo,Consolas,monospace;font-size:.85rem;word-break:break-all;}"
         << ".ok{color:#0a7a2f;font-weight:bold;} .fail{color:#b3261e;font-weight:bold;}"
         << ".meta td:first-child{font-weight:bold;width:220px;}"
         << "nav{background:#f5f5f5;border:1px solid #ddd;border-radius:6px;padding:.75rem 1.25rem;margin-top:1rem;}"
         << "nav a{display:block;padding:.15rem 0;} nav a.sub{padding-left:1.25rem;font-size:.9rem;}"
         << "</style></head><body>";

    html << "<h1>SIM/USIM Forensic Acquisition Report</h1>";

    html << "<nav>"
         << "<a href=\"#case-info\">Case information</a>";
    if (result.refusedUnauthorized) {
        html << "<a href=\"#acquisition-refused\">Acquisition refused</a>";
    }
    html << "<a href=\"#tool-provenance\">Tool provenance</a>"
         << "<a href=\"#acquisition-results\">Acquisition results</a>"
         << "<a class=\"sub\" href=\"#evidence-zip\">Evidence zip</a>"
         << "<a class=\"sub\" href=\"#interpreted-values\">Interpreted values</a>"
         << "<a class=\"sub\" href=\"#extracted-files\">Extracted files</a>"
         << "<a href=\"#chain-of-custody\">Chain of custody &amp; integrity</a>"
         << "<a href=\"#log\">Acquisition log</a>"
         << "</nav>";

    html << "<h2 id=\"case-info\">Case information</h2><table class=\"meta\">"
         << "<tr><td>Case identifier</td><td>" << escapeHtml(result.caseMetadata.caseIdentifier) << "</td></tr>"
         << "<tr><td>Piece / exhibit number</td><td>" << escapeHtml(result.caseMetadata.pieceNumber) << "</td></tr>"
         << "<tr><td>Operator</td><td>" << escapeHtml(result.caseMetadata.operatorName) << "</td></tr>"
         << "<tr><td>Authorization confirmed</td><td class=\""
         << (result.caseMetadata.authorizationConfirmed ? "ok" : "fail") << "\">"
         << (result.caseMetadata.authorizationConfirmed ? "Yes" : "No") << "</td></tr>"
         << "<tr><td>Notes</td><td>" << escapeHtml(result.caseMetadata.examinerNotes) << "</td></tr>"
         << "</table>";

    if (result.refusedUnauthorized) {
        html << "<h2 id=\"acquisition-refused\">Acquisition refused</h2><table class=\"meta\">"
             << "<tr><td>Reason</td><td class=\"fail\">Authorization was not confirmed; the tool "
                "never connected to the card.</td></tr></table>";
    }

    html << "<h2 id=\"tool-provenance\">Tool provenance</h2><table class=\"meta\">"
         << "<tr><td>Tool</td><td>" << kToolName << " v" << kToolVersion << "</td></tr>"
         << "<tr><td>Repository</td><td><a href=\"" << kRepoWeb << "\">" << kRepoWeb << "</a></td></tr>"
         << "<tr><td>Platform</td><td>" << escapeHtml(result.platform) << "</td></tr>"
         << "<tr><td>Executable SHA-256</td><td><code>"
         << (result.toolExeSha256 ? *result.toolExeSha256 : std::string("(unavailable)"))
         << "</code></td></tr>"
         << "</table>";

    html << "<h2 id=\"acquisition-results\">Acquisition results</h2>";

    html << "<h3 id=\"evidence-zip\">Evidence zip</h3><table class=\"meta\">"
         << "<tr><td>File</td><td>" << escapeHtml(zipFileName) << "</td></tr>"
         << "<tr><td>SHA-256</td><td><code>" << zipInfo.sha256 << "</code></td></tr>"
         << "<tr><td>Note</td><td>The zip's own hash cannot be stored inside itself (that would "
            "change the zip and invalidate the hash); this report and manifest.json are the "
            "record of it.</td></tr>"
         << "</table>";

    html << "<h3 id=\"interpreted-values\">Interpreted values</h3>"
         << "<table><tr><th>Field</th><th>Value</th><th>Status</th><th>Note</th></tr>";
    for (const auto& field : buildKeyResults(result)) {
        bool found = field.status == "found";
        html << "<tr><td>" << field.name << "</td><td><code>"
             << (found ? escapeHtml(field.value) : "") << "</code></td>"
             << "<td class=\"" << (found ? "ok" : "") << "\">" << field.status << "</td>"
             << "<td>" << escapeHtml(field.note) << "</td></tr>";
    }
    html << "</table>";

    html << "<h3 id=\"extracted-files\">Extracted files (" << result.files.size() << ")</h3>"
         << "<table><tr><th>Path</th><th>File ID</th><th>Structure</th>"
         << "<th>Size (bytes)</th><th>SHA-256</th><th>Interpreted value</th><th>Flags</th></tr>";
    for (const auto& f : result.files) {
        const char* structure = "?";
        switch (f.structure) {
            case apdu::FileStructure::Transparent: structure = "transparent"; break;
            case apdu::FileStructure::LinearFixed: structure = "linear-fixed"; break;
            case apdu::FileStructure::Cyclic: structure = "cyclic"; break;
            default: structure = "unknown"; break;
        }
        char idHex[8];
        std::snprintf(idHex, sizeof(idHex), "%04X", f.fileId);

        html << "<tr><td>" << escapeHtml(f.path) << "</td><td><code>" << idHex << "</code></td>"
             << "<td>" << structure << "</td><td>" << fileSizeOf(f) << "</td>"
             << "<td><code>" << f.sha256 << "</code></td>"
             << "<td>" << (f.interpretedValue ? escapeHtml(*f.interpretedValue) : "") << "</td>"
             << "<td class=\""
             << ((f.structureUnknown || f.sizeMismatch || f.sensitive) ? "fail" : "") << "\">";
        if (f.structureUnknown) html << "structure unknown ";
        if (f.sizeMismatch) html << "size mismatch ";
        if (f.sensitive) html << "cryptographic key material - content withheld from disk ";
        html << "</td></tr>";
    }
    html << "</table>";

    html << "<h2 id=\"chain-of-custody\">Chain of custody &amp; integrity</h2><table class=\"meta\">"
         << "<tr><td>Acquisition started</td><td>" << isoTimestamp(result.startedAt) << "</td></tr>"
         << "<tr><td>Acquisition finished</td><td>" << isoTimestamp(result.finishedAt) << "</td></tr>"
         << "<tr><td>Workstation</td><td>" << escapeHtml(result.workstationHostname)
         << " (user: " << escapeHtml(result.workstationUser) << ")</td></tr>"
         << "<tr><td>Reader</td><td>" << escapeHtml(result.readerName) << "</td></tr>"
         << "<tr><td>ATR</td><td><code>" << atrHex(result.atr) << "</code></td></tr>"
         << "<tr><td>ICCID</td><td><code>" << escapeHtml(result.iccid) << "</code></td></tr>"
         << "<tr><td>Acquisition mode</td><td>"
         << (result.mode == AcquisitionMode::FullDump ? "Full dump (PIN verified)" : "ICCID only (no PIN)")
         << "</td></tr>";
    if (result.cancelled) {
        html << "<tr><td>Cancelled by operator</td><td class=\"fail\">Yes — these are partial "
                "results; the walk was stopped before completion</td></tr>";
    }
    if (result.pinAttempted) {
        bool ok = result.pinResult == ChvResult::Correct;
        html << "<tr><td>PIN entered</td><td><code>******</code> "
                "(educational purpose — PIN value not disclosed)</td></tr>"
             << "<tr><td>PIN verification</td><td class=\"" << (ok ? "ok" : "fail") << "\">"
             << chvResultString(result.pinResult) << "</td></tr>";
        if (result.chv1AttemptsBeforeVerify.has_value()) {
            html << "<tr><td>CHV1 attempts remaining (before verify)</td><td>"
                 << *result.chv1AttemptsBeforeVerify << "</td></tr>";
        }
    }
    html << "<tr><td>Read-only acquisition</td><td class=\"ok\">Yes - no UPDATE BINARY/RECORD, "
            "INVALIDATE or REHABILITATE command is ever issued by this tool</td></tr>"
         << "<tr><td>Note</td><td>VERIFY CHV (PIN check) is the only card-state-affecting "
            "operation performed; a wrong PIN decrements the card's own retry counter, which "
            "is why the counter is captured before the attempt (see above).</td></tr>";
    if (result.integrityRereadPerformed) {
        bool ok = result.integrityRereadMatches;
        html << "<tr><td>ICCID re-read integrity check</td><td class=\"" << (ok ? "ok" : "fail")
             << "\">" << (ok ? "MATCH" : "MISMATCH") << " (first: <code>" << result.iccidSha256First
             << "</code>, re-read: <code>" << result.iccidSha256Reread << "</code>)</td></tr>";
    }
    if (result.verifyRequested) {
        bool ok = result.verifyPerformed && result.verifyMismatches.empty();
        html << "<tr><td>Full verification pass</td><td class=\"" << (ok ? "ok" : "fail") << "\">"
             << (result.verifyPerformed
                     ? (result.verifyMismatches.empty()
                            ? "Every acquired file was re-read and matched its first hash"
                            : std::to_string(result.verifyMismatches.size()) + " file(s) MISMATCHED on re-read")
                     : "Requested but not completed")
             << "</td></tr>";
        if (!result.verifyMismatches.empty()) {
            html << "<tr><td>Mismatched files</td><td>";
            for (auto& p : result.verifyMismatches) html << "<code>" << escapeHtml(p) << "</code><br>";
            html << "</td></tr>";
        }
    }
    html << "</table>";

    html << "<h2 id=\"log\">Acquisition log</h2><table><tr><th>#</th><th>Message</th></tr>";
    for (size_t i = 0; i < result.log.size(); ++i) {
        html << "<tr><td>" << (i + 1) << "</td><td>" << escapeHtml(result.log[i]) << "</td></tr>";
    }
    html << "</table>";

    html << "</body></html>";

    std::ofstream out(htmlPath, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to open '" + htmlPath + "' for writing");
    }
    out << html.str();
}

} // namespace forandsim::output
