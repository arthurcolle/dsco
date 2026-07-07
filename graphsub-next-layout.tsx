import type { Metadata } from 'next';
import { ClerkProvider } from '@clerk/nextjs';
import './globals.css';

export const metadata: Metadata = {
  metadataBase: new URL('https://graphsub.com'),
  title: 'GraphSub - Agent Graph Control Plane',
  description:
    'Sub-millisecond graph operations, real-world capsules, replayable proof, and competitive graph benchmarks for agent systems.',
  keywords: [
    'graph database',
    'agent infrastructure',
    'AI infrastructure',
    'graph benchmarks',
    'PuppyGraph',
    'Neo4j',
    'Memgraph',
    'TigerGraph',
    'operational proof',
  ],
  authors: [{ name: 'Distributed Systems Corporation' }],
  openGraph: {
    title: 'GraphSub - Agent Graph Control Plane',
    description:
      'Sub-millisecond graph operations, real-world capsules, replayable proof, and competitive graph benchmarks for agent systems.',
    url: 'https://graphsub.com',
    siteName: 'GraphSub',
    images: [
      {
        url: '/og-image.png',
        width: 1200,
        height: 630,
        alt: 'GraphSub - Agent Graph Control Plane',
      },
    ],
    locale: 'en_US',
    type: 'website',
  },
  twitter: {
    card: 'summary_large_image',
    title: 'GraphSub - Agent Graph Control Plane',
    description:
      'Sub-millisecond graph operations, real-world capsules, replayable proof, and competitive graph benchmarks.',
    images: ['/og-image.png'],
  },
};

const hasClerkKeys = !process.env.NEXT_PUBLIC_CLERK_PUBLISHABLE_KEY?.includes('placeholder');

function ConditionalClerkProvider({ children }: { children: React.ReactNode }) {
  if (hasClerkKeys) {
    return <ClerkProvider>{children}</ClerkProvider>;
  }
  return <>{children}</>;
}

export default function RootLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <ConditionalClerkProvider>
      <html lang="en">
        <body className="antialiased">{children}</body>
      </html>
    </ConditionalClerkProvider>
  );
}
