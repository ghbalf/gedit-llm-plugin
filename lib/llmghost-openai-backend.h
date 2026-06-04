#pragma once

#include "llmghost-backend.h"

G_BEGIN_DECLS

typedef enum
{
  LLM_GHOST_OPENAI_MODE_COMPLETIONS,   /* /v1/completions, native FIM via suffix */
  LLM_GHOST_OPENAI_MODE_CHAT,          /* /v1/chat/completions, prompt-engineered FIM */
} LlmGhostOpenAIMode;

G_END_DECLS
