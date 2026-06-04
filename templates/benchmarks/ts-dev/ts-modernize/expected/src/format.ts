// Legacy-style formatting utilities. Modernize to strict ESM idioms
// (named exports, const/let, ===, real types, async/await, array methods)
// WITHOUT changing behavior.

namespace FormatImpl {
  export function formatList(items: any[]): string {
    var out = "";
    for (var i = 0; i < items.length; i++) {
      if (out != "") {
        out = out + ", ";
      }
      out = out + String(items[i]);
    }
    return out;
  }

  export function sum(nums: any[]): number {
    var total = 0;
    for (var i = 0; i < nums.length; i++) {
      total = total + nums[i];
    }
    return total;
  }

  export function delayedEcho(msg: string): Promise<string> {
    return Promise.resolve(msg).then(function (m) {
      return "echo: " + m;
    });
  }
}

export default FormatImpl;
