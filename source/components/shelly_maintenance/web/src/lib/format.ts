export function num(v: number | null | undefined, decimals: number, unit: string): string {
  return v === null || v === undefined ? '–' : `${v.toFixed(decimals)} ${unit}`;
}

export function temp(c: number | null | undefined): string {
  return c === null || c === undefined ? '–' : `${c.toFixed(1)} °C`;
}
