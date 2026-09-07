#pragma once
template <typename... Args>
inline void discardLog(Args&&...) {}
#define LOG_ERR(...) discardLog(__VA_ARGS__)
#define LOG_DBG(...) discardLog(__VA_ARGS__)
