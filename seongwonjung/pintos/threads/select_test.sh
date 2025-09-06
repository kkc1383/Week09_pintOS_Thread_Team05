#!/usr/bin/env bash

# 스크립트가 있는 디렉터리 (pintos/src/threads/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
CONFIG_FILE="${SCRIPT_DIR}/.test_config"

# 설정 파일 읽기
declare -A test_args
declare -A test_dirs
tests=()
while IFS=':' read -r name args dir; do
    # 주석이나 빈 줄 건너뛰기
    [[ -z "${name// /}" || "${name##\#}" != "$name" ]] && continue
    name=$(echo "$name" | xargs)
    tests+=("$name")
    test_args["$name"]=$(echo "$args" | xargs)
    test_dirs["$name"]=$(echo "$dir" | xargs)
done < "$CONFIG_FILE"

# 빌드 디렉터리 확인 및 생성
if [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory not found. Building Pintos..."
    make -C "${SCRIPT_DIR}"
fi

# 테스트 메뉴 표시
echo "=== Pintos Threads Tests ==="
for i in "${!tests[@]}"; do
    printf " %2d) %s\n" "$((i+1))" "${tests[i]}"
done

# 사용자에게 테스트 번호 입력받기
read -p "Enter test numbers (e.g., '1 3 5' or '2-4'): " input

# 입력된 번호를 테스트 이름으로 변환
selected_tests=()
for token in ${input//,/ }; do
    if [[ "$token" =~ ^([0-9]+)-([0-9]+)$ ]]; then
        for i in $(seq "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}"); do
            selected_tests+=("${tests[$i-1]}")
        done
    else
        selected_tests+=("${tests[$token-1]}")
    fi
done

# 선택된 테스트 실행
passed=()
failed=()
cd "$BUILD_DIR" || exit 1

for test_name in "${selected_tests[@]}"; do
    echo "--- Running: $test_name ---"
    result_file="${test_dirs[$test_name]}/${test_name}.result"
    
    # 이전 결과 파일 삭제
    rm -f "$result_file" "${result_file%.result}.output"

    if make "$result_file" ARGS="${test_args[$test_name]}" > /dev/null 2>&1 && grep -q "PASS" "$result_file"; then
        echo "PASS"
        passed+=("$test_name")
    else
        echo "FAIL"
        failed+=("$test_name")
    fi
done

# 최종 결과 요약
echo
echo "=== Test Summary ==="
echo "Passed: ${#passed[@]}"
for t in "${passed[@]}"; do echo "  - $t"; done
echo "Failed: ${#failed[@]}"
for t in "${failed[@]}"; do echo "  - $t"; done