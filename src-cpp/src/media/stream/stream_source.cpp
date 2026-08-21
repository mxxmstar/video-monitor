#include <chrono>
#include <cstddef>
#include <thread>

#include "common/log/logger.h"
#include "media/stream/stream_source.h"

MediaStreamSource::MediaStreamSource(const std::string& stream_id)
    : stream_id_(stream_id) {
}

MediaStreamSource::~MediaStreamSource() {
}


void MediaStreamSource::SetSession(std::shared_ptr<MediaStreamSession> session) {
    session_ = std::move(session);
}

bool MediaStreamSource::Start() {
    if (!session_) {
        LOG_ERROR("[{}] Start() rejected: session_ is null", stream_id_);
        return false;
    }
    applyConfig();
}

void MediaStreamSource::Stop() {
}


void MediaStreamSource::applyConfig() {
    if (!session_) {        
        return;
    }
    session_->ApplyConfig();
}

