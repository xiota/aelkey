#pragma once

namespace SignalHandler {

void handle(int sig);
void install();
void reraise();

}  // namespace SignalHandler
