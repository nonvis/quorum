// HTTP API entry point for the toy service.
import { greet } from "@toy/core";

export function start(): void {
  console.log(greet("api"));
}
