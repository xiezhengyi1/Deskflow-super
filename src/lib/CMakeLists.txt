# SPDX-FileCopyrightText: (C) 2024 Deskflow Developers
# SPDX-FileCopyrightText: (C) 2012 - 2024 Symless Ltd
# SPDX-FileCopyrightText: (C) 2009 - 2012 Nick Bolton
# SPDX-License-Identifier: MIT

add_subdirectory(arch)
add_subdirectory(base)
add_subdirectory(client)
add_subdirectory(common)
add_library(file_relay STATIC
  deskflow/FileRelayWindow.cpp
  deskflow/FileRelayWindow.h
  deskflow/FileTransferSession.cpp
  deskflow/FileTransferSession.h
)
target_include_directories(file_relay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(file_relay PUBLIC Qt6::Widgets Qt6::Network)
add_subdirectory(deskflow)
add_subdirectory(io)
add_subdirectory(mt)
add_subdirectory(net)
add_subdirectory(platform)
add_subdirectory(server)
add_subdirectory(gui)
