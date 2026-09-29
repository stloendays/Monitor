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
