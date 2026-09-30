# Monitor Hub UI rules

The Monitor Hub separates **operational information** from **helper text** so the main window stays dense and readable.

## 1. Always-visible information

Keep these visible because they are part of the user's current operational state:

- project/task name;
- health/status;
- current situation and progress;
- last/next check time;
- job/task IDs and selected-task metadata;
- scientific/runtime parameters for the selected task;
- errors, warnings and items that require a user decision;
- dynamic execution state such as “查询中…”, “回答中…”, “上次刷新 …”;
- primary action buttons.

Never hide an error, warning, required decision, or live execution state inside a tooltip.

## 2. Hover-only helper text

Explanatory text that is not itself a current state belongs in a tooltip:

- what a button does;
- side effects or safety notes for an action;
- keyboard shortcuts;
- color legends;
- “how to use this panel” instructions;
- explanations of read-only behavior, context cost, or file-opening behavior.

Attach the tooltip directly to the relevant functional button. If an explanation applies to a whole panel rather than one action, use a small **“ⓘ 说明”** button in that panel and show the explanation only on hover.

Do not leave long instructional paragraphs permanently visible beside buttons.

## 3. Tooltip behavior

The Python UI implements one shared tooltip mechanism:

- show after roughly 350 ms of pointer hover;
- disappear when the pointer leaves;
- disappear immediately when the button is pressed;
- wrap long text and keep it inside the screen bounds;
- never execute an action merely because the tooltip is shown.

This is a presentation layer only; it must not alter monitor state.

## 4. Confirmations are not helper text

State-changing operations such as pause/resume/run-now may still show a confirmation dialog **after the user clicks the action**. Confirmation dialogs are part of the operation flow, not passive helper text.

## 5. C++ / Qt migration rule

The future Qt 6 UI must preserve the same distinction:

- primary state and actionable data stay visible;
- helper/explanatory text uses button hover tooltips;
- panel-level explanations use an info button;
- warnings/errors/required decisions remain persistent and visually prominent.

The C++/Qt port should not reintroduce persistent help paragraphs that the Python UI has moved into tooltips.


## 6. Visual design system

The Qt desktop UI uses a restrained warm-neutral visual system inspired by modern AI workspaces without copying any product-specific trade dress.

### Surfaces

- application background: warm off-white;
- navigation/sidebar: slightly darker neutral surface;
- operational cards and tables: white;
- separators: low-contrast warm gray;
- avoid default bright-blue Qt chrome unless it communicates real state.

### Hierarchy

Use typography and spacing before color:

- product/page title;
- current operational status;
- section title;
- body/detail text;
- muted metadata/helper text.

The UI should feel calm even when many projects are present.

### Status color

Reserve semantic color for actual operational meaning:

- green: healthy or successfully completed;
- blue: active child-agent/background handling;
- amber: attention or stale state;
- red: monitor/error state;
- gray: paused/neutral metadata.

Do not color large surfaces solely for decoration.

### Interaction

- selected navigation items use a soft neutral fill;
- primary action uses a dark neutral button;
- secondary actions remain light;
- tables have comfortable row height, subtle separators, and no heavy grid;
- cards use moderate radius and light borders rather than shadows;
- raw evidence/code paths use a subdued inset surface.

### Context header

The top context header follows navigation state:

- Overview shows **Monitor Hub / Agent Operations** and aggregate health;
- project tabs show the selected project, its health, headline, and runner state.

This prevents a selected project's title from visually leaking into the cross-project Overview.


## 7. Quick-debug action bar

The desktop UI exposes a persistent **快速调试** bar for low-risk inspection tasks.

The default buttons are:

- refresh normalized monitor state;
- open the registered monitor directory;
- open the registered status file;
- open the selected-task or project monitor log;
- open the selected task directory;
- open the selected-task or first available project result;
- jump to background Agent/takeover records;
- open the global project registry;
- open the Hub runtime-data directory;
- open the detached-job root;
- copy the selected task or runner command without executing it;
- copy a bounded diagnostic context for troubleshooting.

Rules:

- these actions are read-only and must not pause, restart, resubmit or change scientific/runtime parameters;
- unavailable actions are disabled instead of producing dead-end clicks;
- task-level paths take precedence where the user has explicitly selected a task;
- project-level paths are the fallback for monitor-level debugging;
- global registry/runtime/job-root buttons make configuration discovery possible without memorizing filesystem layout;
- the actions are split into two rows: current project/task inspection and global configuration/runtime inspection;
- each button carries a tooltip explaining when to use it and its side effects;
- the help tooltip recommends the beginner flow: refresh → status file → monitor log → background records → results;
- copying diagnostics must not include tokens, passwords, API keys or private credentials.

State-changing controls such as pause/resume/run-now belong in a separate, clearly marked control surface with explicit authority and confirmation rules.
