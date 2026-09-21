import type { Metadata } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import "./globals.css";

const geistSans = Geist({
  variable: "--font-geist-sans",
  subsets: ["latin"],
});

const geistMono = Geist_Mono({
  variable: "--font-geist-mono",
  subsets: ["latin"],
});

export const metadata: Metadata = {
  metadataBase: new URL("https://lume.example"),
  title: "Lume — seu contexto, presente",
  description: "Um assistente pessoal que carrega o contexto para você.",
  openGraph: {
    title: "Lume — seu contexto, presente",
    description: "Um assistente pessoal que carrega o contexto para você.",
    images: [{ url: "/og.png", width: 1730, height: 909, alt: "Lume — Seu contexto, presente." }],
  },
  twitter: {
    card: "summary_large_image",
    title: "Lume — seu contexto, presente",
    description: "Um assistente pessoal que carrega o contexto para você.",
    images: ["/og.png"],
  },
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="pt-BR">
      <body
        className={`${geistSans.variable} ${geistMono.variable} antialiased`}
      >
        {children}
      </body>
    </html>
  );
}
