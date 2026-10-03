// Quick access menu of bdex-framegen (Steam Deck / SteamOS Game Mode): the
// settings of the layer's configuration that matter in game, written by the
// backend (main.py), which the layer reads again within a second.
import { ButtonItem, DropdownItem, PanelSection, PanelSectionRow, ToggleField, staticClasses } from "@decky/ui";
import { callable, definePlugin } from "@decky/api";
import { useEffect, useState } from "react";
import { FaFilm } from "react-icons/fa";

type Status = { game: string; line: string; refresh_hz: number } | null;
type State = {
  generate: boolean;
  multiplier: string;  // "off" | "auto" | "adaptive" | "2".."8"
  mode: string;        // "interpolate" | "hybrid" | "extrapolate"
  paused: boolean;
  hud: boolean;
  installed?: boolean;
  status?: Status;
};

const getState = callable<[], State>("get_state");
const setState = callable<[Partial<State>], State>("set_state");

const MULTIPLIERS = [
  { data: "auto", label: "Auto (screen refresh)" },
  { data: "adaptive", label: "Adaptive (every refresh)" },
  { data: "2", label: "×2" },
  { data: "3", label: "×3" },
  { data: "4", label: "×4" },
];
const MODES = [
  { data: "interpolate", label: "Interpolation" },
  { data: "hybrid", label: "Hybrid (×3/×4: lower latency)" },
  { data: "extrapolate", label: "Extrapolation (lowest latency)" },
];

function Content() {
  const [state, set] = useState<State | null>(null);
  const refresh = () => getState().then(set).catch(() => {});
  useEffect(() => {
    refresh();
    const t = setInterval(refresh, 1000);
    return () => clearInterval(t);
  }, []);
  const change = (c: Partial<State>) => {
    if (state) set({ ...state, ...c });
    setState(c).then((s) => set((old) => ({ ...(old ?? s), ...s }))).catch(() => {});
  };
  if (!state) return <PanelSection title="bdex-framegen"><PanelSectionRow>…</PanelSectionRow></PanelSection>;
  const status = state.status;
  return (
    <PanelSection title="Frame generation">
      {state.installed === false && (
        <PanelSectionRow>
          <div>The layer is not installed (install.sh, or the package): nothing will happen.</div>
        </PanelSectionRow>
      )}
      <PanelSectionRow>
        <ToggleField
          label="Every game"
          description="Vulkan and Proton games generate frames from their start"
          checked={state.generate}
          onChange={(v) => change({ generate: v })}
        />
      </PanelSectionRow>
      <PanelSectionRow>
        <DropdownItem
          label="Frames per game frame"
          rgOptions={MULTIPLIERS}
          selectedOption={state.multiplier === "off" ? "auto" : state.multiplier}
          disabled={!state.generate}
          onChange={(o) => change({ multiplier: o.data })}
        />
      </PanelSectionRow>
      <PanelSectionRow>
        <DropdownItem
          label="Mode"
          rgOptions={MODES}
          selectedOption={state.mode}
          disabled={!state.generate}
          onChange={(o) => change({ mode: o.data })}
        />
      </PanelSectionRow>
      <PanelSectionRow>
        <ToggleField
          label="Paused"
          description="Suspends the generation in the running game"
          checked={state.paused}
          disabled={!state.generate}
          onChange={(v) => change({ paused: v })}
        />
      </PanelSectionRow>
      <PanelSectionRow>
        <ToggleField label="FPS counter" checked={state.hud} onChange={(v) => change({ hud: v })} />
      </PanelSectionRow>
      <PanelSectionRow>
        <div style={{ fontSize: "12px" }}>
          {status ? `${status.game}: ${status.line}` : "No game running with the layer"}
        </div>
      </PanelSectionRow>
      <PanelSectionRow>
        <ButtonItem layout="below" onClick={refresh}>Refresh</ButtonItem>
      </PanelSectionRow>
    </PanelSection>
  );
}

export default definePlugin(() => ({
  name: "bdex-framegen",
  titleView: <div className={staticClasses.Title}>bdex-framegen</div>,
  content: <Content />,
  icon: <FaFilm />,
  onDismount() {},
}));
