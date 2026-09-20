import js from '@eslint/js'
import tseslint from 'typescript-eslint'
import globals from 'globals'
import prettier from 'eslint-config-prettier'

export default tseslint.config(
  {
    ignores: [
      '**/node_modules/**',
      '**/build*/**',
      '**/managed_components/**',
      '.gea/**',
      // Emscripten's generated glue for the WASM panel.
      'web/dist/**',
      'third_party/**',
      '**/.env*',
      '**/.npmrc*',
    ],
  },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  {
    files: ['**/*.{js,mjs,cjs}', '*.{js,mjs,cjs}'],
    languageOptions: { globals: globals.node },
  },
  {
    files: ['src/ui/**/*.{ts,tsx}', 'src/preview/**/*.{ts,tsx}'],
    languageOptions: { globals: globals.browser },
  },
  {
    rules: {
      '@typescript-eslint/no-unused-vars': [
        'error',
        { argsIgnorePattern: '^_', varsIgnorePattern: '^_' },
      ],
    },
  },
  prettier,
)
