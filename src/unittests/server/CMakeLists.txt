# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

if(WIN32)
  set(extra_libs version app mt net)
endif()

create_test(
  NAME LocalCursorTests
  DEPENDS server
  LIBS base arch io mt net platform app ${extra_libs}
  SOURCE LocalCursorTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/server"
)

create_test(
  NAME ServerConfigTests
  DEPENDS server
  LIBS base arch ${extra_libs}
  SOURCE ServerConfigTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/server"
)

create_test(
  NAME ServerTests
  DEPENDS server
  LIBS base arch ${extra_libs}
  SOURCE ServerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/server"
)

create_test(
  NAME ClientProxyTests
  DEPENDS server
  LIBS base arch io mt net platform app ${extra_libs}
  SOURCE ClientProxyTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/server"
)

create_test(
  NAME FileTransferTests
  DEPENDS app
  LIBS server base arch io mt net platform ${extra_libs}
  SOURCE FileTransferTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/server"
)

create_test(
  NAME FileRelayTests
  DEPENDS app
  LIBS server base arch io mt net platform ${extra_libs}
  SOURCE FileRelayTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/server"
)
