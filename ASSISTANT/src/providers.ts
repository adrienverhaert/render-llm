import { query } from '@anthropic-ai/claude-agent-sdk';
import type {
  CanUseTool,
  PermissionResult,
  SDKMessage,
  SDKUserMessage,
} from '@anthropic-ai/claude-agent-sdk';
import { codexQuery } from './providers-codex.js';

export type ProviderId = 'codex' | 'claude';
export type ProviderQueryArgs = Parameters<typeof query>[0] & {
  options: Parameters<typeof query>[0]['options'] & { renderLlmTurnId?: string };
};
export type ProviderCanUseTool = CanUseTool;
export type ProviderPermissionResult = PermissionResult;
export type ProviderToolMessage = {
  type: 'render_llm_tool';
  name: string;
  phase: 'start' | 'end';
  ok?: boolean;
  ms?: number;
};
export type ProviderMessage = SDKMessage | ProviderToolMessage;
export type ProviderQueryFn = (args: ProviderQueryArgs) => AsyncIterable<ProviderMessage>;
export type ProviderUserMessage = SDKUserMessage;

export interface VisualAgentProvider {
  id: ProviderId;
  query: ProviderQueryFn;
  supportsVision(model: string): boolean;
}

const CLAUDE_VISION_MODELS = new Set([
  'claude-haiku-4-5',
  'claude-sonnet-5',
  'claude-opus-5',
]);

const CLAUDE_PROVIDER: VisualAgentProvider = {
  id: 'claude',
  query: (args) => query(args),
  supportsVision(model) {
    return CLAUDE_VISION_MODELS.has(model);
  },
};

const CODEX_VISION_MODELS = new Set(['gpt-5.6-luna', 'gpt-5.6-terra', 'gpt-6-astra']);

const CODEX_PROVIDER: VisualAgentProvider = {
  id: 'codex',
  query: codexQuery,
  supportsVision(model) {
    return CODEX_VISION_MODELS.has(model);
  },
};

export function getVisualAgentProvider(id: string): VisualAgentProvider {
  if (id === 'codex') return CODEX_PROVIDER;
  if (id === 'claude') return CLAUDE_PROVIDER;
  throw new Error(`Unsupported visual-agent provider: ${id}`);
}
