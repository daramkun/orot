---
name: commit
description: Git 커밋 명령
model: haiku
---

## Commit
Git에 커밋할 때는 아래와 같은 규칙으로 진행한다.

1. Commit 제목에는 120자 이내로 해당 커밋 내용의 요약을 작성한다.
2. Commit 제목 앞에는 fix, docs, feat, refactor, perf 등 용도에 맞는 prefix를 붙인다.
3. 커밋 메시지 본문에는 커밋 내용의 각 작업별 요약을 리스트로, 이후 상세 설명을 작성한다. 본문의 크기는 1000자를 넘지 않도록 한다. (어쩔 수 없는 경우에만 넘긴다)
4. 커밋 메시지의 가장 마지막에 작업에 사용된 LLM 모델명을 Co-Authored-By로 추가한다. (내부명 말고 일반적으로 표시하는 이름)
