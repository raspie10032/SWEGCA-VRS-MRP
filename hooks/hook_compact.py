import json
# SessionStart(compact) guidance hook. M6-D1 (doc VRS_MCP_libvrs_v1.md 14.14): saying only that the tool exists left the
# model trusting the summary; the guidance now says the summary is not the original and when to fetch the original.
msg = ("이 문맥은 압축되었다. 위의 압축 요약은 원문이 아니며 빠지거나 바뀐 내용이 있을 수 있다. "
       "압축 전 대화의 사실·결정·수치·사용자 원문에 기대어 답하거나 행동하기 전에는, 먼저 vrs-lib 의 vrs_restore 도구(인자 없음)로 "
       "압축으로 빠진 원문(사용자 발화·답)을 받아 확인하라. 결과가 여러 쪽이면 next 값을 start 로 넘겨 이어 받는다. "
       "항목에 붙은 원본 위치의 바이트 범위는 vrs_read 로 읽을 수 있다.")
print(json.dumps({"hookSpecificOutput": {"hookEventName": "SessionStart", "additionalContext": msg}}, ensure_ascii=False))
