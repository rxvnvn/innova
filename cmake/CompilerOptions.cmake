# Compiler and linker options, applied per target through innova_apply_common_options().

include(CheckCXXCompilerFlag)

set(INNOVA_CXX_WARNINGS
    -Wall -Wextra
    -Wno-ignored-qualifiers
    -Wno-format
    -Wno-unused-parameter
    -Wno-literal-suffix
    -Wno-deprecated-declarations
    -Wno-deprecated-copy)

set(INNOVA_HARDENING_COMPILE -fstack-protector-all -Wstack-protector)
set(INNOVA_HARDENING_LINK -Wl,-z,relro -Wl,-z,now)

function(innova_apply_common_options target)
  target_compile_options(${target} PRIVATE
      ${INNOVA_CXX_WARNINGS}
      -pthread
      -fno-stack-protector)
  # re-apply the real hardening after -fno-stack-protector, matching the
  # historical build order (the last -fstack-protector* wins)
  target_compile_options(${target} PRIVATE ${INNOVA_HARDENING_COMPILE})
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-O2>)
    target_compile_definitions(${target} PRIVATE _FORTIFY_SOURCE=2)
  endif()
  if(NOT WIN32 AND NOT APPLE)
    target_link_options(${target} PRIVATE ${INNOVA_HARDENING_LINK})
  endif()
  target_link_libraries(${target} PRIVATE Threads::Threads)
endfunction()
