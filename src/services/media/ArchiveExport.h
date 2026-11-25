#pragma once

#include "services/media/MediaArchiveService.h"
#include <optional>

namespace HuhuCapture::Media {

struct ExportResult {
    Failure failure;
    QString path;
    bool committed{};
};

struct ExportDestination {
    QString path;
    std::optional<Platform::FileSnapshot> existing;
};
struct DestinationResult { Failure failure; ExportDestination destination; };

// Inspect on a worker BEFORE asking to replace an existing file. The GUI must
// confirm this exact snapshot; a missing target never implies overwrite consent.
DestinationResult inspectExportDestination(const QString& selectedPath);

// Worker-only Save As of a completed archive entry. The caller must obtain the
// user's destination/overwrite confirmation first. Preserve the durable archive
// on every outcome; QSaveFile supplies atomic replacement, not a second durable
// archive contract. A managed destination also requires its removal reservation.
ExportResult exportArchiveFile(const MediaArchiveService& archive, const Entry& source,
    const ExportDestination& confirmedDestination, std::stop_token stop = {});

} // namespace HuhuCapture::Media
