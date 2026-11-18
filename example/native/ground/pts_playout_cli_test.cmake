# Reject malformed options before loading configuration, opening SDL, or starting a session.
function(check_cli expected_code expected_text)
    execute_process(COMMAND "${VIDEO_TRANSMISSION_GROUND}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 3)
    if(NOT "${result}" STREQUAL "${expected_code}" OR
       NOT "${output}${error}" MATCHES "${expected_text}")
        message(FATAL_ERROR "args=${ARGN}: result=${result}, output=${output}, error=${error}")
    endif()
endfunction()

check_cli(0 "playout-delay-ms" --help)
check_cli(1 "--fullscreen requires --gui" --config /unused.yaml --fullscreen)
check_cli(1 "require --gui" --config /unused.yaml --playout-delay-ms 250)
check_cli(2 "usage:" --config /unused.yaml --gui --playout-delay-ms)
foreach(value IN ITEMS -1 1.5 nope)
    check_cli(1 "non-negative integers" --config /unused.yaml --gui --playout-delay-ms "${value}")
endforeach()
check_cli(1 "exceeds limit" --config /unused.yaml --gui --playout-delay-ms 10001)
check_cli(1 "invalid bounded PTS" --config /unused.yaml --gui
    --playout-delay-ms 400 --playout-max-delay-ms 400)
check_cli(1 "invalid bounded PTS" --config /unused.yaml --gui --playout-max-delay-ms 0)
check_cli(1 "invalid bounded PTS" --config /unused.yaml --gui --playout-max-frames 0)
check_cli(1 "invalid bounded PTS" --config /unused.yaml --gui --playout-max-frames 121)
