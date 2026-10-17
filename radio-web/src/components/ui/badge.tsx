import * as React from 'react';
import { cva, type VariantProps } from 'class-variance-authority';
import { cn } from '@/lib/utils';

const badgeVariants = cva('inline-flex items-center gap-1 rounded-full px-2 py-0.5 text-xs font-medium [&_svg]:size-3', {
  variants: {
    variant: {
      default: 'bg-muted text-muted-fg',
      accent: 'bg-accent-soft text-accent',
      outline: 'border text-muted-fg',
      ok: 'bg-ok/15 text-ok',
      danger: 'bg-danger/15 text-danger',
    },
  },
  defaultVariants: { variant: 'default' },
});

export function Badge({
  className,
  variant,
  ...props
}: React.HTMLAttributes<HTMLSpanElement> & VariantProps<typeof badgeVariants>) {
  return <span className={cn(badgeVariants({ variant }), className)} {...props} />;
}
